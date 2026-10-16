











#pragma once

#include "sdkconfig.h"
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_ML_ENABLE_CELLULAR













esp_err_t ml_at_socket_init(void);







void ml_at_socket_deinit(void);




bool ml_at_socket_is_ready(void);














int ml_at_socket(int domain, int type, int protocol);







int ml_at_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);








ssize_t ml_at_send(int fd, const void *buf, size_t len, int flags);







ssize_t ml_at_recv(int fd, void *buf, size_t len, int flags);







ssize_t ml_at_sendto(int fd, const void *buf, size_t len, int flags,
                      const struct sockaddr *dest_addr, socklen_t addrlen);







ssize_t ml_at_recvfrom(int fd, void *buf, size_t len, int flags,
                        struct sockaddr *src_addr, socklen_t *addrlen);







int ml_at_close(int fd);








int ml_at_bind(int fd, const struct sockaddr *addr, socklen_t addrlen);








int ml_at_setsockopt(int fd, int level, int optname,
                      const void *optval, socklen_t optlen);






int ml_at_fcntl(int fd, int cmd, int arg);












int ml_at_select(int nfds, fd_set *readfds, fd_set *writefds,
                  fd_set *exceptfds, struct timeval *timeout);











int ml_at_getaddrinfo(const char *hostname, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res);




void ml_at_freeaddrinfo(struct addrinfo *res);






#define ML_AT_SOCK_FD_BASE  32
#define ML_AT_SOCK_MAX      10




bool ml_at_socket_is_at_fd(int fd);





ssize_t ml_at_write(int fd, const void *buf, size_t len);





ssize_t ml_at_read(int fd, void *buf, size_t len);

#else 


static inline bool ml_at_socket_is_ready(void) { return false; }
static inline bool ml_at_socket_is_at_fd(int fd) { (void)fd; return false; }

#endif 

#ifdef __cplusplus
}
#endif
