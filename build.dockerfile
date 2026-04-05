FROM alpine:latest

RUN apk add --no-cache gcc musl-dev make ncurses-dev ncurses-static
