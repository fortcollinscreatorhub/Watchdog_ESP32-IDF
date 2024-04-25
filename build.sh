#!/bin/bash

set -e
cd "$(dirname "$0")"

# v4.1.4 works
# v4.2.5 works
# v4.3.7 works
# v4.4.7 works
image="espressif/idf:v4.4.7"
inside_command=(idf.py build)
docker_opts=()
docker_opts+=(-it)
docker_opts+=(--rm)
docker_opts+=(-u "$(id -u):$(id -g)")
docker_opts+=(-v "${HOME}:${HOME}")
docker_opts+=(-e "HOME=${HOME}")
docker_opts+=(-v "$(pwd):$(pwd)")
docker_opts+=(-w "$(pwd)")

if [ "$#" -gt 0 ]; then
    inside_command=("$@")
fi

for arg in "${inside_command[@]}"; do
    case "${arg}" in
    /dev/*)
        if [ -c "${arg}" ]; then
            docker_opts+=("--device=${arg}")
        fi
        ;;
    esac
done

exec docker run \
    "${docker_opts[@]}" \
    "${image}" \
    "${inside_command[@]}"
