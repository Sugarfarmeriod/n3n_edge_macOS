/**
 * (C) 2007-22 - ntop.org and contributors
 * Copyright (C) 2023-25 Hamish Coleman
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not see see <http://www.gnu.org/licenses/>
 *
 */

#ifdef __APPLE__

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <n3n/logging.h>  // for traceEvent
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>   // for in_addr_t
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "n2n.h"


extern char **environ;

#define N2N_OSX_TAPDEVICE_SIZE 32
#define N2N_OSX_FETH_PAIRS 128


struct n2n_osx_tap_state {
    int bpf_fd;
    uint8_t *bpf_buffer;
    u_int bpf_buffer_size;
    int packet_tx_fd;
    int stop_rx_fd;
    int stop_tx_fd;
    int cleanup_tx_fd;
    bool owns_feth;
    bool reader_started;
    pid_t cleanup_pid;
    pthread_t reader;
    devstr_t peer_name;
};


static int run_command (char *const argv[]) {

    pid_t pid;
    int status;
    int rc = posix_spawn(&pid, argv[0], NULL, NULL, argv, environ);

    if(rc != 0) {
        errno = rc;
        return -1;
    }

    do {
        rc = waitpid(pid, &status, 0);
    } while((rc < 0) && (errno == EINTR));

    if(rc < 0) {
        return -1;
    }

    if(!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
        errno = EIO;
        return -1;
    }

    return 0;
}


static int run_ifconfig (char *const args[]) {

    int count;
    char *argv[16];

    argv[0] = "/sbin/ifconfig";
    for(count = 0; args[count] && (count < 14); count++) {
        argv[count + 1] = args[count];
    }
    if(args[count]) {
        errno = E2BIG;
        return -1;
    }
    argv[count + 1] = NULL;

    return run_command(argv);
}


static int get_interface_mac (const char *name, n2n_mac_t mac) {

    struct ifaddrs *addresses = NULL;
    struct ifaddrs *scan;
    int rc = -1;

    if(getifaddrs(&addresses) != 0) {
        return -1;
    }

    for(scan = addresses; scan; scan = scan->ifa_next) {
        if(!scan->ifa_addr || strcmp(scan->ifa_name, name)) {
            continue;
        }
        if(scan->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *link = (const struct sockaddr_dl *)scan->ifa_addr;
            if(link->sdl_alen == sizeof(n2n_mac_t)) {
                memcpy(mac, LLADDR(link), sizeof(n2n_mac_t));
                rc = 0;
                break;
            }
        }
    }

    freeifaddrs(addresses);
    return rc;
}


static int configure_interface (const char *name,
                                struct n2n_ip_subnet v4subnet,
                                const char *device_mac,
                                int mtu) {

    char address[INET_ADDRSTRLEN];
    char netmask[INET_ADDRSTRLEN];
    char mtu_text[16];
    struct in_addr addr;
    struct in_addr mask;

    /* n3n's configuration parser stores IPv4 addresses in network order. */
    addr.s_addr = v4subnet.net_addr;
    mask.s_addr = htonl(bitlen2mask(v4subnet.net_bitlen));

    if(!inet_ntop(AF_INET, &addr, address, sizeof(address)) ||
       !inet_ntop(AF_INET, &mask, netmask, sizeof(netmask))) {
        return -1;
    }

    if(device_mac && device_mac[0]) {
        char *mac_args[] = {(char *)name, "ether", (char *)device_mac, NULL};
        if(run_ifconfig(mac_args) != 0) {
            traceEvent(TRACE_ERROR, "Unable to set MAC address on %s", name);
            return -1;
        }
    }

    snprintf(mtu_text, sizeof(mtu_text), "%d", mtu);
    char *addr_args[] = {
        (char *)name,
        "inet",
        address,
        "netmask",
        netmask,
        "mtu",
        mtu_text,
        "up",
        NULL
    };

    return run_ifconfig(addr_args);
}


static int open_legacy_tap (tuntap_dev *device,
                            struct n2n_ip_subnet v4subnet,
                            const char *device_mac,
                            int mtu) {

    char path[N2N_OSX_TAPDEVICE_SIZE];
    int index;

    for(index = 0; index < 255; index++) {
        snprintf(path, sizeof(path), "/dev/tap%d", index);
        device->fd = open(path, O_RDWR);
        if(device->fd >= 0) {
            snprintf(device->dev_name, sizeof(device->dev_name), "tap%d", index);
            break;
        }
    }

    if(device->fd < 0) {
        return -1;
    }

    if(configure_interface(device->dev_name, v4subnet, device_mac, mtu) != 0 ||
       get_interface_mac(device->dev_name, device->mac_addr) != 0) {
        close(device->fd);
        device->fd = -1;
        return -1;
    }

    traceEvent(TRACE_NORMAL, "Using legacy TAP device %s", device->dev_name);
    return device->fd;
}


static int select_feth_pair (const char *requested,
                             char *visible,
                             size_t visible_size,
                             char *peer,
                             size_t peer_size) {

    unsigned int index;

    if(requested && !strncmp(requested, "feth", 4)) {
        char extra;
        if((sscanf(requested, "feth%u%c", &index, &extra) != 1) ||
           (index >= (N2N_OSX_FETH_PAIRS * 2)) ||
           (index & 1)) {
            errno = EINVAL;
            return -1;
        }
        if(if_nametoindex(requested) ||
           ((size_t)snprintf(peer, peer_size, "feth%u", index + 1) >= peer_size) ||
           if_nametoindex(peer)) {
            errno = EEXIST;
            return -1;
        }
        snprintf(visible, visible_size, "%s", requested);
        return 0;
    }

    for(index = 0; index < (N2N_OSX_FETH_PAIRS * 2); index += 2) {
        snprintf(visible, visible_size, "feth%u", index);
        snprintf(peer, peer_size, "feth%u", index + 1);
        if(!if_nametoindex(visible) && !if_nametoindex(peer)) {
            return 0;
        }
    }

    errno = ENOSPC;
    return -1;
}


static int open_bpf (const char *interface) {

    char path[sizeof("/dev/bpf000")];
    struct ifreq request;
    u_int enabled = 1;
    u_int see_sent = 0;
    u_int data_link = 0;
    int fd = -1;
    int index;

    for(index = 0; index < 256; index++) {
        snprintf(path, sizeof(path), "/dev/bpf%d", index);
        fd = open(path, O_RDWR);
        if(fd >= 0) {
            break;
        }
        if(errno != EBUSY) {
            return -1;
        }
    }
    if(fd < 0) {
        return -1;
    }

    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);

    if((ioctl(fd, BIOCSETIF, &request) < 0) ||
       (ioctl(fd, BIOCIMMEDIATE, &enabled) < 0) ||
       (ioctl(fd, BIOCSHDRCMPLT, &enabled) < 0) ||
       (ioctl(fd, BIOCSSEESENT, &see_sent) < 0) ||
       (ioctl(fd, BIOCGDLT, &data_link) < 0) ||
       (data_link != DLT_EN10MB)) {
        close(fd);
        if(data_link != DLT_EN10MB) {
            errno = EPROTONOSUPPORT;
        }
        return -1;
    }

    return fd;
}


static void *bpf_reader (void *context) {

    struct n2n_osx_tap_state *state = context;
    struct pollfd descriptors[2];

    descriptors[0].fd = state->bpf_fd;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = state->stop_rx_fd;
    descriptors[1].events = POLLIN;

    while(1) {
        int poll_rc = poll(descriptors, 2, -1);
        if(poll_rc < 0) {
            if(errno == EINTR) {
                continue;
            }
            break;
        }
        if(descriptors[1].revents) {
            break;
        }
        if(!(descriptors[0].revents & POLLIN)) {
            continue;
        }

        ssize_t length = read(state->bpf_fd,
                              state->bpf_buffer,
                              state->bpf_buffer_size);
        if(length <= 0) {
            if((length < 0) && (errno == EINTR)) {
                continue;
            }
            break;
        }

        uint8_t *cursor = state->bpf_buffer;
        uint8_t *end = state->bpf_buffer + length;
        while(cursor < end) {
            struct bpf_hdr *header = (struct bpf_hdr *)cursor;
            size_t record_size;

            if((size_t)(end - cursor) < sizeof(*header) ||
               header->bh_hdrlen > (size_t)(end - cursor) ||
               header->bh_caplen > (size_t)(end - cursor) - header->bh_hdrlen) {
                traceEvent(TRACE_WARNING, "Discarding malformed BPF record");
                break;
            }

            if(send(state->packet_tx_fd,
                    cursor + header->bh_hdrlen,
                    header->bh_caplen,
                    0) < 0 &&
               (errno != EAGAIN) && (errno != EWOULDBLOCK)) {
                break;
            }

            record_size = BPF_WORDALIGN(header->bh_hdrlen + header->bh_caplen);
            if(!record_size || (record_size > (size_t)(end - cursor))) {
                break;
            }
            cursor += record_size;
        }
    }

    return NULL;
}


static int start_cleanup_helper (struct n2n_osx_tap_state *state,
                                 const char *visible,
                                 const char *peer) {

    int cleanup_pipe[2];
    pid_t pid;

    if(pipe(cleanup_pipe) != 0) {
        return -1;
    }

    pid = fork();
    if(pid < 0) {
        close(cleanup_pipe[0]);
        close(cleanup_pipe[1]);
        return -1;
    }

    if(pid == 0) {
        long max_fd;

        close(cleanup_pipe[1]);
        /*
         * Leave the edge's foreground process group. Otherwise a terminal
         * Control-C kills this helper before it can remove the interfaces.
         */
        if(setsid() < 0) {
            _exit(127);
        }
        if(dup2(cleanup_pipe[0], STDIN_FILENO) < 0) {
            _exit(127);
        }

        max_fd = sysconf(_SC_OPEN_MAX);
        if(max_fd < 0) {
            max_fd = 1024;
        }
        for(int fd = STDERR_FILENO + 1; fd < max_fd; fd++) {
            close(fd);
        }

        execl("/bin/sh",
              "sh",
              "-c",
              "read ignored || true; "
              "/sbin/ifconfig \"$1\" destroy >/dev/null 2>&1 || true; "
              "/sbin/ifconfig \"$2\" destroy >/dev/null 2>&1 || true",
              "n3n-feth-cleanup",
              visible,
              peer,
              (char *)NULL);
        _exit(127);
    }

    close(cleanup_pipe[0]);
    state->cleanup_tx_fd = cleanup_pipe[1];
    state->cleanup_pid = pid;
    return 0;
}


static void destroy_feth_pair (struct n2n_osx_tap_state *state,
                               const char *visible) {

    if(!state || !state->owns_feth) {
        return;
    }

    if(state->cleanup_tx_fd >= 0) {
        close(state->cleanup_tx_fd);
        state->cleanup_tx_fd = -1;
        while((waitpid(state->cleanup_pid, NULL, 0) < 0) && (errno == EINTR)) {
            /* Retry interrupted wait. */
        }
        state->cleanup_pid = 0;
    } else {
        char *visible_args[] = {(char *)visible, "destroy", NULL};
        char *peer_args[] = {state->peer_name, "destroy", NULL};
        run_ifconfig(visible_args);
        run_ifconfig(peer_args);
    }
    state->owns_feth = false;
}


static int open_feth_tap (tuntap_dev *device,
                          const char *requested,
                          struct n2n_ip_subnet v4subnet,
                          const char *device_mac,
                          int mtu) {

    struct n2n_osx_tap_state *state = NULL;
    char visible[sizeof(device->dev_name)];
    char peer[sizeof(device->dev_name)];
    char mtu_text[16];
    int packet_pair[2] = {-1, -1};
    int stop_pair[2] = {-1, -1};

    if(geteuid() != 0) {
        traceEvent(TRACE_ERROR, "The macOS feth/BPF backend must run as root");
        errno = EPERM;
        return -1;
    }

    if(select_feth_pair(requested,
                        visible,
                        sizeof(visible),
                        peer,
                        sizeof(peer)) != 0) {
        traceEvent(TRACE_ERROR, "Unable to select an unused feth interface pair: %s", strerror(errno));
        return -1;
    }

    state = calloc(1, sizeof(*state));
    if(!state) {
        return -1;
    }
    state->bpf_fd = -1;
    state->packet_tx_fd = -1;
    state->stop_rx_fd = -1;
    state->stop_tx_fd = -1;
    state->cleanup_tx_fd = -1;
    snprintf(state->peer_name, sizeof(state->peer_name), "%s", peer);
    snprintf(device->dev_name, sizeof(device->dev_name), "%s", visible);

    char *create_visible[] = {visible, "create", NULL};
    char *create_peer[] = {peer, "create", NULL};
    char *pair_args[] = {visible, "peer", peer, NULL};

    if(run_ifconfig(create_visible) != 0) {
        traceEvent(TRACE_ERROR, "Unable to create feth interfaces");
        goto fail;
    }
    state->owns_feth = true;
    if(run_ifconfig(create_peer) != 0) {
        traceEvent(TRACE_ERROR, "Unable to create feth interfaces");
        goto fail;
    }

    if(run_ifconfig(pair_args) != 0) {
        traceEvent(TRACE_ERROR, "Unable to pair %s with %s", visible, peer);
        goto fail;
    }

    snprintf(mtu_text, sizeof(mtu_text), "%d", mtu);
    char *peer_up_args[] = {peer, "mtu", mtu_text, "up", NULL};
    if((configure_interface(visible, v4subnet, device_mac, mtu) != 0) ||
       (run_ifconfig(peer_up_args) != 0) ||
       (get_interface_mac(visible, device->mac_addr) != 0)) {
        traceEvent(TRACE_ERROR, "Unable to configure feth interfaces");
        goto fail;
    }

    if(start_cleanup_helper(state, visible, peer) != 0) {
        traceEvent(TRACE_ERROR, "Unable to start privileged feth cleanup helper");
        goto fail;
    }

    state->bpf_fd = open_bpf(peer);
    if(state->bpf_fd < 0) {
        traceEvent(TRACE_ERROR, "Unable to attach BPF to %s: %s", peer, strerror(errno));
        goto fail;
    }
    if((ioctl(state->bpf_fd, BIOCGBLEN, &state->bpf_buffer_size) < 0) ||
       !(state->bpf_buffer = malloc(state->bpf_buffer_size))) {
        traceEvent(TRACE_ERROR, "Unable to allocate BPF read buffer");
        goto fail;
    }

    if((socketpair(AF_UNIX, SOCK_DGRAM, 0, packet_pair) != 0) ||
       (socketpair(AF_UNIX, SOCK_DGRAM, 0, stop_pair) != 0)) {
        goto fail;
    }

    int flags = fcntl(packet_pair[1], F_GETFL, 0);
    if((flags < 0) || (fcntl(packet_pair[1], F_SETFL, flags | O_NONBLOCK) < 0)) {
        goto fail;
    }

    device->fd = packet_pair[0];
    state->packet_tx_fd = packet_pair[1];
    state->stop_rx_fd = stop_pair[0];
    state->stop_tx_fd = stop_pair[1];
    packet_pair[0] = packet_pair[1] = -1;
    stop_pair[0] = stop_pair[1] = -1;

    if(pthread_create(&state->reader, NULL, bpf_reader, state) != 0) {
        traceEvent(TRACE_ERROR, "Unable to start BPF reader thread");
        goto fail;
    }
    state->reader_started = true;

    device->osx_priv = state;
    traceEvent(TRACE_NORMAL,
               "Using built-in macOS feth/BPF TAP backend: %s <-> %s",
               visible,
               peer);
    return device->fd;

fail:
    if(packet_pair[0] >= 0) close(packet_pair[0]);
    if(packet_pair[1] >= 0) close(packet_pair[1]);
    if(stop_pair[0] >= 0) close(stop_pair[0]);
    if(stop_pair[1] >= 0) close(stop_pair[1]);
    if(device->fd >= 0) close(device->fd);
    if(state->packet_tx_fd >= 0) close(state->packet_tx_fd);
    if(state->stop_rx_fd >= 0) close(state->stop_rx_fd);
    if(state->stop_tx_fd >= 0) close(state->stop_tx_fd);
    if(state->bpf_fd >= 0) close(state->bpf_fd);
    free(state->bpf_buffer);
    destroy_feth_pair(state, visible);
    free(state);
    device->fd = -1;
    return -1;
}


int tuntap_open (tuntap_dev *device,
                 char *dev,
                 uint8_t address_mode, /* unused */
                 struct n2n_ip_subnet v4subnet,
                 const char * device_mac,
                 int mtu,
                 int ignored) {

    (void)address_mode;
    (void)ignored;

    memset(device, 0, sizeof(*device));
    device->fd = -1;
    device->ip_addr = v4subnet.net_addr;
    device->mtu = mtu;

    if(open_legacy_tap(device, v4subnet, device_mac, mtu) >= 0) {
        return device->fd;
    }

    traceEvent(TRACE_INFO,
               "No legacy /dev/tap device is available; trying the built-in macOS feth/BPF backend");
    return open_feth_tap(device, dev, v4subnet, device_mac, mtu);
}


int tuntap_read (struct tuntap_dev *tuntap, unsigned char *buf, int len) {

    if(tuntap->osx_priv) {
        return recv(tuntap->fd, buf, len, 0);
    }
    return read(tuntap->fd, buf, len);
}


int tuntap_write (struct tuntap_dev *tuntap, unsigned char *buf, int len) {

    struct n2n_osx_tap_state *state = tuntap->osx_priv;
    if(state) {
        return write(state->bpf_fd, buf, len);
    }
    return write(tuntap->fd, buf, len);
}


void tuntap_close (struct tuntap_dev *tuntap) {

    struct n2n_osx_tap_state *state = tuntap->osx_priv;

    if(!state) {
        if(tuntap->fd >= 0) {
            close(tuntap->fd);
            tuntap->fd = -1;
        }
        return;
    }

    if(state->reader_started) {
        char stop = 1;
        write(state->stop_tx_fd, &stop, sizeof(stop));
        pthread_join(state->reader, NULL);
    }

    close(tuntap->fd);
    close(state->packet_tx_fd);
    close(state->stop_rx_fd);
    close(state->stop_tx_fd);
    close(state->bpf_fd);
    free(state->bpf_buffer);
    destroy_feth_pair(state, tuntap->dev_name);
    free(state);

    tuntap->fd = -1;
    tuntap->osx_priv = NULL;
}

// fill out the ip_addr value from the interface, called to pick up dynamic address changes
void tuntap_get_address (struct tuntap_dev *tuntap) {

    (void)tuntap;
    // no action
}


#endif /* __APPLE__ */
