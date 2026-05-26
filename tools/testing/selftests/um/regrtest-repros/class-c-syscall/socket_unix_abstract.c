/* Class C syscall repro: AF_UNIX abstract namespace bind+connect. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(void)
{
	int srv = socket(AF_UNIX, SOCK_STREAM, 0);
	if (srv < 0) {
		printf("REPRO: socket_unix_abstract FAIL srv_socket_errno=%d\n", errno);
		return 0;
	}

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	const char name[] = "test_uml_abstract";
	addr.sun_path[0] = '\0';
	memcpy(&addr.sun_path[1], name, sizeof(name) - 1);
	socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 1 + (sizeof(name) - 1);

	if (bind(srv, (struct sockaddr *)&addr, alen) < 0) {
		printf("REPRO: socket_unix_abstract FAIL bind_errno=%d\n", errno);
		close(srv);
		return 0;
	}
	if (listen(srv, 1) < 0) {
		printf("REPRO: socket_unix_abstract FAIL listen_errno=%d\n", errno);
		close(srv);
		return 0;
	}

	int cli = socket(AF_UNIX, SOCK_STREAM, 0);
	if (cli < 0) {
		printf("REPRO: socket_unix_abstract FAIL cli_socket_errno=%d\n", errno);
		close(srv);
		return 0;
	}
	if (connect(cli, (struct sockaddr *)&addr, alen) < 0) {
		printf("REPRO: socket_unix_abstract FAIL connect_errno=%d\n", errno);
		close(cli);
		close(srv);
		return 0;
	}

	close(cli);
	close(srv);
	printf("REPRO: socket_unix_abstract PASS\n");
	return 0;
}
