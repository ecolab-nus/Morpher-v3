# Running Morpher using Docker
Docker packages Morpher and its dependencies into a reusable image. A container is a running instance of that image. Build once, then open containers whenever you need.

## 1. Before you start 
Install and start Docker for your machine (Docker Engine for Linux, Docker Desktop for Windows or MacOS).

Commands use Bash-compatible syntax. Host terminal means your Mac/Linux terminal or Windows WSL terminal. Test your installation by running the following command in your terminal:

```bash
docker run hello-world
```
If successful, you should see something similar to the following output:

```bash
Hello from Docker!
This message shows that your installation appears to be working correctly.

To generate this message, Docker took the following steps:
 1. The Docker client contacted the Docker daemon.
 2. The Docker daemon pulled the "hello-world" image from the Docker Hub.
    (amd64)
 3. The Docker daemon created a new container from that image which runs the
    executable that produces the output you are currently reading.
 4. The Docker daemon streamed that output to the Docker client, which sent it
    to your terminal.

To try something more ambitious, you can run an Ubuntu container with:
 $ docker run -it ubuntu bash

Share images, automate workflows, and more with a free Docker ID:
 https://hub.docker.com/

For more examples and ideas, visit:
 https://docs.docker.com/get-started/

```

## 2. Build the image
In your host terminal, obtain the repository if you do not already have it:
```bash
git clone https://github.com/ecolab-nus/Morpher-v3.git
cd Morpher-v3
```

From the repository root containing the `Dockerfile` run:
```bash
docker build -t morpher-v3:llvm18 .
```
The final `.` selects this directory as the build context. `-t` tags the image with a name. Docker installs dependencies and compiles all three components. Later builds reuse unchanged layers. Local builds do not require a Docker Hub account.

## 3. Open and use Morpher with an interactive container
From the repository root in your host terminal:
```bash
mkdir -p generated workspace

docker run --rm -it \
  --name morpher-v3-dev \
  -v "$PWD/generated:/opt/morpher/generated" \
  -v "$PWD/workspace:/opt/morpher/workspace" \
  morpher-v3:llvm18 bash
```

> Containers are transient and once you terminate a container session, any content generated during runtime is destroyed. That is, if you run the container using the same image again, the outputs and changes from the previous container you terminated are not propagated. In order to preserve outputs, we create and link the `workspace` and `generated` folders from inside the container to our own versions of the same folders in the host machine.

| Argument | What it does |
| -------- | ------------ |
| `-it` | Opens terminal inside container in interactive mode. |
| `--name morpher-v3-dev` | Gives the container a predictable name for other Docker commands. |
| `--rm` | 	Automatically removes the container when its main shell exits. |
| `-v "host:container"` | Shares the specified host folder with the container. |
| `morpher-v3:llvm18 bash` | Starts Bash using the image you built. |

You are now inside the container, in `/opt/morpher`, with Morpher already compiled. The prompt typically looks like `root@<container-id>:/opt/morpher#`. You can now run a sample morpher benchmark to ensure everything is setup:

```bash
python -u run_morpher.py \
  morpher_benchmarks/array_add/array_add.c array_add
```

This generates DFGs, selects a mapping, and runs functional simulation. Success reports positive matches, zero mismatches, and Artifact simulation test passed!!!. See the [README.md](../README.md#quick-start) for other kernels/configurations.

## 4. Exit, reopen, and inspect
To leave the container, run inside its shell:
```bash
exit
```
With `--rm`, the container is removed automatically. Your image and the two shared output folders remain. Repeat the `docker run` command to open a fresh container.

These are other useful Docker commands you can use in the host terminal:

| Task | Command |
| -------- | ------------ |
| List running containers | `docker ps` |
| List all containers | `docker ps -a` |
| Open a second shell in the running container | `docker exec -it morpher-v3-dev bash` |
| Stop the container from another terminal | `docker stop morpher-v3-dev` |
| List available images | `docker image ls` |

To retain a container between sessions, omit `--rm` when creating it. After `exit`, resume it with `docker start -ai morpher-v3-dev`, remove it while stopped with `docker rm morpher-v3-dev`. Names must be unique - resume or remove an existing container before reusing its name. Exiting a second `docker exec` shell only closes that shell.

## 5. Where do the files go
The two bind mounts keep results available on your host:
| Host folder | Container folder | Contents |
| ----------- | ---------------- | -------- |
| `generated/` | `/opt/morpher/generated/` | Run summaries, including `result.csv`. |
| `workspace/` | `/opt/morpher/workspace/` | DFGs, traces, mappings, simulator artifacts, and detailed logs. |

Source files are copied into the image at build time. Rebuild the image after editing the source files in the host. Edits inside a container disappear on removal. To edit configurations on the host, add `-v "$PWD/config:/opt/morpher/config"` before the image name in `docker run`. Mounting the entire checkout over `/opt/morpher` hides the compiled build directories.

On Linux, add `--user "$(id -u):$(id -g)" --env HOME=/tmp` before the image name to create outputs as your host user.

### Developing with the full repository mounted
For active development, share your entire host checkout with the container. Run from the repository root in your host terminal, after exiting/removing any container with the same name:
```bash
docker run --rm -it \
  --name morpher-v3-dev \
  -v "$PWD:/opt/morpher" \
  morpher-v3:llvm18 bash
```

This exposes your host files at `/opt/morpher`, it does not copy the image's files onto your host. The mount hides the compiled files bundled there, so build the mounted checkout inside the container before its first run:

```bash
bash build_all.sh
python -u run_morpher.py \
  morpher_benchmarks/array_add/array_add.c array_add
```

Source edits, build files, and results now persist directly in your host checkout, even after `exit` removes the container. After source edits, rebuild the affected component, for example `cmake --build Morpher_CGRA_Mapper/build --parallel 2`. Rebuild the image when changing its system or Python dependencies.

The root `.gitignore` excludes `generated/, workspace/,` and all three component build directories, so normal `git add .` skips them. `.dockerignore` controls build context contents only. 

## 6. What does the Dockerfile do
| Instruction or section | Meaning |
|---|---|
| `FROM ubuntu:24.04` | Starts with an Ubuntu Linux filesystem. Docker Desktop supplies a Linux VM on Mac and Windows. |
| `SHELL [...]` | Uses Bash for build commands; `pipefail` propagates failures within pipelines. |
| System-package `RUN` | Installs CMake, compilers, LLVM/Clang 18, Polly headers, multilib support, Graphviz, and utilities. Removing APT lists saves space. |
| `VIRTUAL_ENV`, `venv`, and `PATH` | Creates a Python environment and makes its Python and the LLVM 18 tools available by name. |
| `MORPHER_LLVM_*` | Points Morpher and CMake to the matching LLVM 18 tools, libraries, and configuration. |
| `PYTHONUNBUFFERED=1` | Makes Python progress output appear promptly. |
| `WORKDIR /opt/morpher` | Sets the working directory for subsequent build commands and new container shells. |
| Requirements `COPY` and `pip install` | Installs Python dependencies in a separate cached layer. |
| `COPY . .` | Copies the local checkout, excluding `.dockerignore` entries. Keep `**/build`, `generated`, and `workspace` excluded. |
| `BUILD_JOBS` and CMake loop | Builds the DFG plugin, mapper, and simulator with bounded compilation parallelism. The empty build type follows the upstream default. |
| `RUN mkdir ...` and `CMD ["/bin/bash"]` | Creates output directories and chooses Bash as the default container command. |

## 7. Rebuild after changes
Save source changes in your host repo, then rebuild from its root:
```bash
docker build -t morpher-v3:llvm18 .
```

Rebuilding updates the image tag, but existing containers keep their original filesystem. Exit/remove the old container and use `docker run` again.

If compilation runs out of memory, reduce parallelism:
```bash
docker build \
  --build-arg BUILD_JOBS=1 -t morpher-v3:llvm18 .
```
For Docker connection errors, check Docker Desktop or the Docker Engine service. For Morpher failures, inspect the detailed log path printed by the runner.