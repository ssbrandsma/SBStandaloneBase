/*
 * Management webserver process entry point.
 *
 * Loads settings, applies command-line overrides and starts http_server.c.
 * The Lua applet supervises this sbwebserver process alongside the local
 * LMS-compatible sbbase service and the sbproxy compatibility proxy.
 */

#include "http_server.h"
#include "settings.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/**
 * @brief Run the program entry point.
 * @param argc Number of command-line arguments.
 * @param argv Command-line argument vector.
 * @return Process exit status.
 */
int main(int argc, char **argv)
{
	Settings s;
	settings_load(&s);
	const char *root = "/usr/share/jive/applets/SBWebserver/web";
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--port") && i + 1 < argc)
			s.port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--web-root") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(argv[i], "--config-dir") && i + 1 < argc) {
#ifdef _WIN32
			_putenv_s("SBWEBSERVER_CONFIG_DIR", argv[++i]);
#else
			setenv("SBWEBSERVER_CONFIG_DIR", argv[++i], 1);
#endif
		}
	printf("SBWebserver listening on 0.0.0.0:%d\n", s.port);
	return http_server_run(s.port, root);
}
