# Deployment

`cppboot package` builds a release of your project, with everything it needs to run on another machine. On Linux, that includes a systemd service with limited access to the system.

```text
cppboot package
```

| | Linux | Windows |
|---|---|---|
| Archive | `dist/<name>-linux.tar.gz` | `dist/<name>-windows.zip` |
| Program | `bin/<name>` | `bin\<name>.exe` |
| Shared libraries | `lib/` | Next to the program |
| Settings and migrations | `cppboot.yaml`, `cppboot-*.yaml` and `db/` | The same |
| Service | `<name>.service`, `<name>.env.example`, `install.sh` and `uninstall.sh` | None |

The server doesn't need CppBoot or a compiler. These limits apply:

- **glibc:** a Linux package includes every library except glibc, so the server needs the same or a newer version than the machine that built it. Build on the oldest system you deploy to.
- **Security updates:** the package includes its own copies of libraries such as OpenSSL. A system update won't fix them, so **build and install a new package when one of them has a security fix.**
- **The processor:** a package only runs on the kind of processor that built it.
- **`cppboot.yaml` is copied as it is:** so don't put secrets in it. The `--postgresql` sample has a local development password, which you should override in the service's settings.

## Install as a Linux service

Copy the archive to the server and unpack it, then run the install script:

```bash
tar -xzf my-api-linux.tar.gz
cd my-api
sudo ./install.sh
```

Then put the settings and secrets in the file it created, and start the service:

```bash
sudo nano /etc/my-api/my-api.env
sudo systemctl start my-api
sudo systemctl status my-api
journalctl -u my-api
```

| Path | Holds | Owned by |
|---|---|---|
| `/opt/my-api` | The program, libraries, `cppboot.yaml` and migrations | root, so the service can't change them |
| `/etc/my-api/my-api.env` | Settings and secrets | Readable by root and the service only |
| `/var/lib/my-api` | Data, such as a SQLite file | The service |

The file `my-api.env` holds [environment variables](configuration.md), which override `cppboot.yaml`. For example, `CPPBOOT_SERVER_HOST=0.0.0.0` makes the service reachable from other machines. All the common ones are there, commented out.

### Upgrade and remove

Run `sudo ./install.sh` from a newer package to upgrade. It keeps your settings and data, and restarts the service if it's running. The service is down during the restart, so requests sent then fail.

`sudo ./uninstall.sh` removes the service and the program. It keeps your settings, data and the service's account, and prints the command that deletes them.

## How the service behaves

| Situation | What happens |
|---|---|
| Starting | `systemctl start` returns once the application is ready, or fails if it couldn't start |
| The machine boots | It starts automatically, once the network is up |
| It crashes | systemd restarts it after 2 seconds. Repeated failures stop that, under systemd's default limits |
| Its settings are invalid (exit code 78) | It isn't restarted, because that wouldn't help. Fix them and run `systemctl start` |
| `systemctl stop` | It stops accepting connections and lets requests finish, for up to 45 seconds |
| Logs | They go to the system journal. Read them with `journalctl -u my-api` |

A missing secret exits with code 1, so it's restarted a few times before systemd gives up.

## What limits the service

It runs as its own account, which can't log in. These limit what a bug or an attacker could do through it, without fixing the bug itself:

- It can't write anywhere except `/var/lib/my-api` and its private `/tmp`.
- It can't read home folders, gain privileges, load kernel modules or change the clock.
- It has no Linux capabilities, and can only use network and local sockets.
- Its system calls are limited to systemd's `@system-service` set.

`systemd-analyze security my-api` shows the result. To change a limit, run `sudo systemctl edit my-api`, which keeps your change through upgrades.

### Ports below 1024

The service has no capabilities, so it can't listen on ports such as 443. **The simplest answer is to keep port 8443 and redirect to it:**

```bash
sudo iptables -t nat -A PREROUTING -p tcp --dport 443 -j REDIRECT --to-ports 8443
```

Or give it only the capability it needs, with `sudo systemctl edit my-api`:

```ini
[Service]
AmbientCapabilities=CAP_NET_BIND_SERVICE
CapabilityBoundingSet=CAP_NET_BIND_SERVICE
```

### HTTPS

Put the certificate and key where the service can read them, then [set them](web.md#https) in `my-api.env`:

```bash
sudo install -m 640 -o root -g my-api fullchain.pem privkey.pem /etc/my-api/
```

Let's Encrypt keeps its keys where other accounts can't read them, so copy them in a renewal hook, and restart the service there too.

## On Windows

The archive holds the program and its libraries. Run it from its folder, so that it finds its settings. Running it as a Windows service isn't supported yet.

## Before going live

- Set `cppboot.server.max_connections` to what your memory can hold. See the [benchmarks](../benchmarks.md).
- Put a [rate limit](security.md#rate-limits) on every route that checks a password.
- Back up `/var/lib/my-api` and `/etc/my-api`, which uninstalling keeps but a failed disk doesn't.
