#include "package_files.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <fstream>
#include <system_error>

namespace cppboot::cli {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMostAccountNameLength = 32;

// The application's folder is read-only to it, and anything it writes goes under /var/lib. Each line
// in the second group removes something a web service doesn't need, so that a bug in the application
// can do less. That includes binding ports below 1024, which the deployment guide explains.
constexpr std::string_view kServiceUnit = R"([Unit]
Description=@NAME@
Wants=network-online.target
After=network-online.target

[Service]
# The service counts as started once the application says it's ready.
Type=notify
User=@USER@
Group=@USER@
WorkingDirectory=/opt/@NAME@
EnvironmentFile=/etc/@NAME@/@NAME@.env
ExecStart=/opt/@NAME@/bin/@NAME@
Restart=on-failure
RestartSec=2
# 78 means the settings are invalid, and restarting won't fix that.
RestartPreventExitStatus=78
# More than cppboot.server.shutdown_timeout_seconds, so that requests can finish.
TimeoutStopSec=45
StateDirectory=@NAME@
LimitNOFILE=65536

NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes
ProtectClock=yes
ProtectHostname=yes
ProtectProc=invisible
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
SystemCallArchitectures=native
SystemCallFilter=@system-service
CapabilityBoundingSet=
UMask=0077

[Install]
WantedBy=multi-user.target
)";

// Everything is commented out, so that a value only applies once someone chooses it. Secrets belong
// here rather than in cppboot.yaml, because only root and the service can read this file.
constexpr std::string_view kEnvironmentFile = R"(# Settings for @NAME@. Restart it after changing them: systemctl restart @NAME@
# Only root and the service can read this file, so secrets belong here.

# Listen on every network address, instead of this machine only.
#CPPBOOT_SERVER_HOST=0.0.0.0
#CPPBOOT_SERVER_PORT=8080

# Serve HTTPS. Make sure the @USER@ account can read both files.
#CPPBOOT_SERVER_TLS_CERTIFICATE_FILE=/etc/@NAME@/fullchain.pem
#CPPBOOT_SERVER_TLS_PRIVATE_KEY_FILE=/etc/@NAME@/privkey.pem

# List your proxy if there's one in front of this service.
#CPPBOOT_SERVER_TRUSTED_PROXIES=10.0.0.5

#CPPBOOT_SERVER_MAX_CONNECTIONS=1024

# Needed by projects that use security. Create one with: openssl rand -base64 48
#CPPBOOT_SECURITY_JWT_SECRET=

# Lets a metrics scraper read /actuator/metrics.
#CPPBOOT_ACTUATOR_METRICS_TOKEN=

# For a project that uses a database.
#CPPBOOT_DATABASE_URL=postgresql://user:password@localhost:5432/@NAME@
)";

constexpr std::string_view kInstallScript = R"sh(#!/bin/sh
# Installs @NAME@ as a systemd service, or upgrades it. Run it as root.
set -eu

name=@NAME@
account=@USER@
source_dir=$(cd "$(dirname "$0")" && pwd)
cd "$source_dir"
install_dir=/opt/$name
config_dir=/etc/$name
settings=$config_dir/$name.env

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this as root, such as with sudo." >&2
    exit 1
fi
if ! command -v systemctl >/dev/null 2>&1; then
    echo "This needs systemd." >&2
    exit 1
fi

if ! id "$account" >/dev/null 2>&1; then
    useradd --system --no-create-home --home-dir "/var/lib/$name" --shell /usr/sbin/nologin "$account"
fi

was_running=no
if systemctl is-active --quiet "$name"; then
    was_running=yes
fi

# The new version is complete before anything replaces the old one.
rm -rf "$install_dir.new" "$install_dir.old"
mkdir -p "$install_dir.new"
for item in bin lib db cppboot.yaml cppboot-*.yaml; do
    if [ -e "$source_dir/$item" ]; then
        cp -R "$source_dir/$item" "$install_dir.new/"
    fi
done
chown -R root:root "$install_dir.new"
chmod -R u=rwX,go=rX "$install_dir.new"
if [ -e "$install_dir" ]; then
    mv "$install_dir" "$install_dir.old"
fi
mv "$install_dir.new" "$install_dir"
rm -rf "$install_dir.old"

# An upgrade keeps your settings.
install -d -m 0750 -o root -g "$account" "$config_dir"
if [ ! -e "$settings" ]; then
    install -m 0640 -o root -g "$account" "$source_dir/$name.env.example" "$settings"
    # The application's folder is read-only to it, so a SQLite file has to live elsewhere.
    if grep -q 'url: sqlite:' "$source_dir/cppboot.yaml"; then
        echo "CPPBOOT_DATABASE_URL=sqlite:/var/lib/$name/$name.db" >>"$settings"
    fi
fi

install -m 0644 "$source_dir/$name.service" "/etc/systemd/system/$name.service"
systemctl daemon-reload
systemctl enable --quiet "$name"

if [ "$was_running" = yes ]; then
    systemctl restart "$name"
    echo "Upgraded and restarted $name."
else
    echo "Installed $name. Next:"
    echo "  1. Put its settings and secrets in $settings"
    echo "  2. systemctl start $name"
    echo "  3. systemctl status $name"
    echo "  Its log is at: journalctl -u $name"
fi
)sh";

constexpr std::string_view kUninstallScript = R"sh(#!/bin/sh
# Removes the @NAME@ service. Its settings and data stay, because deleting them can't be undone.
set -eu

name=@NAME@

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this as root, such as with sudo." >&2
    exit 1
fi

systemctl disable --now --quiet "$name" 2>/dev/null || true
rm -f "/etc/systemd/system/$name.service"
systemctl daemon-reload
rm -rf "/opt/$name"

echo "Removed $name. To delete its settings, data and account too, run:"
echo "  rm -rf /etc/$name /var/lib/$name && userdel @USER@"
)sh";

constexpr std::string_view kLinuxReadme = R"(# @NAME@

This folder has everything @NAME@ needs, apart from the C library, so the server needs the same or a
newer version of glibc than the machine that built it. It doesn't need CppBoot or a compiler.

To install it as a systemd service, which runs under its own account with limited access:

    sudo ./install.sh

Run the same command with a newer package to upgrade. Your settings and data stay. To remove it:

    sudo ./uninstall.sh

To try it without installing, run this from this folder:

    ./bin/@NAME@
)";

constexpr std::string_view kWindowsReadme = R"(# @NAME@

This folder has everything @NAME@ needs. It doesn't need CppBoot or a compiler. Run it from this
folder, so that it finds its settings:

    bin\@NAME@.exe
)";

constexpr std::string_view kRuntimeLibrariesScript = R"(# Copies an executable's shared libraries next to it. Run with: cmake -DEXE=... -DLIB_DIR=... -P <file>
if(WIN32)
    set(search_directories $ENV{PATH})
else()
    set(search_directories "")
endif()

# glibc has to match the kernel and other parts of the system, so it has to come from there.
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${EXE}"
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    DIRECTORIES ${search_directories}
    PRE_EXCLUDE_REGEXES
        [[^ld-linux.*]] [[^libc\.so.*]] [[^libm\.so.*]] [[^libdl\.so.*]] [[^libpthread\.so.*]]
        [[^librt\.so.*]] [[^libresolv\.so.*]] [[^libnss_.*]] [[^api-ms-.*]] [[^ext-ms-.*]]
    POST_EXCLUDE_REGEXES [[.*[/\\][Ss]ystem32[/\\].*]] [[.*[/\\][Ww]indows[/\\].*]]
)

if(unresolved)
    message(FATAL_ERROR "Can't find these libraries: ${unresolved}")
endif()

foreach(library IN LISTS resolved)
    file(COPY "${library}" DESTINATION "${LIB_DIR}" FOLLOW_SYMLINK_CHAIN)
endforeach()
)";

std::string replaceAll(std::string text, std::string_view placeholder, std::string_view value) {
    std::size_t position = 0;
    while ((position = text.find(placeholder, position)) != std::string::npos) {
        text.replace(position, placeholder.size(), value);
        position += value.size();
    }
    return text;
}

Result<void> writeFile(const fs::path& path, std::string_view content, bool executable = false) {
    {
        std::ofstream output{path, std::ios::binary};
        output << content;
        output.close();
        if (!output) {
            return Error{ErrorCode::IoError, "Cannot write " + path.generic_string()};
        }
    }

    if (executable) {
        std::error_code error;
        fs::permissions(path,
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, error);
        if (error) {
            return Error{ErrorCode::IoError, "Cannot make " + path.generic_string() +
                                                 " executable: " + error.message()};
        }
    }
    return {};
}

}  // namespace

Result<std::string> serviceAccountName(std::string_view project_name) {
    if (project_name.size() > kMostAccountNameLength) {
        return Error{ErrorCode::InvalidArgument,
                     "A service's name can have at most 32 characters, because it's also its "
                     "account name"};
    }

    std::string name{project_name};
    std::transform(name.begin(), name.end(), name.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return name;
}

Result<void> writePackageFiles(const fs::path& stage, std::string_view project_name,
                               bool for_linux) {
    auto account = serviceAccountName(project_name);
    if (!account) {
        return account.error();
    }

    const auto fill = [&](std::string_view content) {
        return replaceAll(replaceAll(std::string{content}, "@USER@", account.value()), "@NAME@",
                          project_name);
    };
    const std::string name{project_name};

    if (!for_linux) {
        return writeFile(stage / "README.md", fill(kWindowsReadme));
    }

    const struct {
        std::string file;
        std::string_view content;
        bool executable;
    } files[] = {
        {"README.md", kLinuxReadme, false},
        {name + ".service", kServiceUnit, false},
        {name + ".env.example", kEnvironmentFile, false},
        {"install.sh", kInstallScript, true},
        {"uninstall.sh", kUninstallScript, true},
    };
    for (const auto& file : files) {
        if (auto written = writeFile(stage / file.file, fill(file.content), file.executable);
            !written) {
            return written;
        }
    }
    return {};
}

std::string_view runtimeLibrariesScript() {
    return kRuntimeLibrariesScript;
}

}  // namespace cppboot::cli
