FROM mcr.microsoft.com/devcontainers/base:alpine-3.22

# Install Docker CLI so the container can talk to the host Docker socket
RUN apk add --no-cache docker-cli docker-cli-compose github-cli python3 py3-pip \
	&& ln -sf /usr/bin/python3 /usr/local/bin/python \
	&& addgroup -S docker \
	&& addgroup vscode docker
