#!/usr/bin/env python3
"""Deploy a StandaloneBase dist ZIP to a Squeezebox Radio over SSH."""

from __future__ import annotations

import argparse
import getpass
import os
import re
import shlex
import sys
import time
import zipfile
from pathlib import Path

try:
    import paramiko
except ImportError:
    sys.stderr.write("deploy_dist requires Paramiko: python -m pip install paramiko\n")
    raise SystemExit(2)


REQUIRED = {
    "StandaloneBaseMeta.lua",
    "StandaloneBaseApplet.lua",
    "config.json",
    "sb-storage-helper",
    "sbubifs-authorized.ko",
    "sbdata-empty.ubifs",
    "storage-setup.sh",
    "storage-boot.sh",
    "patch-squeezeplay-artwork.sh",
}
EXECUTABLES = {
    "sbbase",
    "sbwebserver",
    "sbproxy",
    "sb-storage-helper",
    "sb-storage-updater",
    "storage-setup.sh",
    "storage-boot.sh",
    "patch-squeezeplay-artwork.sh",
}
# Install through the merged UnionFS path only. On SqueezeOS, /mnt/storage is
# the writable branch beneath that union. Addressing /mnt/storage/usr through
# the merged namespace can create /mnt/storage/mnt/storage/usr recursively and
# consume the small UBIFS volume with a duplicate applet tree.
TARGETS = ("/usr/share/jive/applets/StandaloneBase",)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="deploy_dist",
        description="Validate and deploy a StandaloneBase dist ZIP over SSH.",
    )
    parser.add_argument("zip_file", type=Path, help="ZIP from the dist directory")
    parser.add_argument("target", help="SSH destination in user@host form")
    parser.add_argument("-p", "--password", help="SSH password; -pPASSWORD is accepted")
    parser.add_argument("--port", type=int, default=22, help="SSH port (default: 22)")
    parser.add_argument(
        "--accept-new-host-key",
        action="store_true",
        help="trust an unknown host key for this run (known hosts are verified by default)",
    )
    parser.add_argument("--no-restart", action="store_true", help="do not restart SqueezePlay")
    parser.add_argument("--dry-run", action="store_true", help="validate locally without connecting")
    return parser.parse_args()


def validate_package(path: Path) -> tuple[list[str], str]:
    path = path.resolve()
    if not path.is_file():
        raise ValueError(f"package not found: {path}")
    match = re.fullmatch(r"StandaloneBase-([0-9]+\.[0-9]+\.[0-9]+)\.zip", path.name)
    if not match:
        raise ValueError("package name must be StandaloneBase-X.Y.Z.zip")
    if path.parent.name.lower() != "dist":
        raise ValueError("package must be located in a dist directory")
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        names = [entry.filename for entry in infos if not entry.is_dir()]
        if len(names) != len(set(names)):
            raise ValueError("package contains duplicate entries")
        if any("/" in name or "\\" in name or name in (".", "..") for name in names):
            raise ValueError("package entries must all be flat file names")
        missing = sorted(REQUIRED.difference(names))
        if missing:
            raise ValueError("package is missing: " + ", ".join(missing))
        bad = [entry.filename for entry in infos if entry.is_dir()]
        if bad:
            raise ValueError("package must not contain directories")
        if archive.testzip() is not None:
            raise ValueError("package CRC validation failed")
    return names, match.group(1)


def split_target(value: str) -> tuple[str, str]:
    if value.count("@") != 1:
        raise ValueError("target must use user@host syntax")
    user, host = value.split("@", 1)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", user):
        raise ValueError("invalid SSH user")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]+", host):
        raise ValueError("invalid SSH host")
    return user, host


class Radio:
    def __init__(self, user: str, host: str, password: str, port: int, accept_new: bool):
        self.user, self.host, self.password, self.port = user, host, password, port
        self.accept_new = accept_new
        self.client = self._new_client()

    def _new_client(self):
        client = paramiko.SSHClient()
        client.load_system_host_keys()
        client.set_missing_host_key_policy(
            paramiko.AutoAddPolicy() if self.accept_new else paramiko.RejectPolicy()
        )
        return client

    def connect(self) -> None:
        self.client.connect(
            self.host,
            port=self.port,
            username=self.user,
            password=self.password,
            look_for_keys=False,
            allow_agent=False,
            timeout=12,
        )

    def run(self, command: str, attempts: int = 3) -> str:
        last: Exception | None = None
        for attempt in range(attempts):
            try:
                if not self.client.get_transport() or not self.client.get_transport().is_active():
                    self.client.close()
                    self.client = self._new_client()
                    self.connect()
                _, stdout, stderr = self.client.exec_command(command)
                output, error = stdout.read().decode(errors="replace"), stderr.read().decode(errors="replace")
                status = stdout.channel.recv_exit_status()
                if status:
                    raise RuntimeError(f"remote command failed ({status}): {error or output}")
                return output
            except Exception as exc:  # Dropbear occasionally closes a session.
                last = exc
                self.client.close()
                if attempt + 1 < attempts:
                    time.sleep(1)
        raise RuntimeError(str(last))

    def upload(self, source: Path, remote_directory: str, remote_name: str) -> None:
        payload = source.read_bytes()
        for attempt in range(3):
            try:
                if not self.client.get_transport() or not self.client.get_transport().is_active():
                    self.connect()
                channel = self.client.get_transport().open_session()
                channel.exec_command("scp -t " + shlex.quote(remote_directory))
                self._scp_ok(channel)
                channel.sendall(f"C0644 {len(payload)} {remote_name}\n".encode())
                self._scp_ok(channel)
                channel.sendall(payload)
                channel.sendall(b"\0")
                self._scp_ok(channel)
                channel.close()
                return
            except Exception:
                self.client.close()
                if attempt == 2:
                    raise
                self.client = self._new_client()
                time.sleep(1)

    @staticmethod
    def _scp_ok(channel) -> None:
        response = channel.recv(1)
        if response != b"\0":
            detail = channel.recv(1024).decode(errors="replace")
            raise RuntimeError("SCP rejected transfer: " + detail)


def main() -> int:
    args = arguments()
    try:
        names, version = validate_package(args.zip_file)
        user, host = split_target(args.target)
    except (ValueError, zipfile.BadZipFile) as exc:
        sys.stderr.write(f"deploy_dist: {exc}\n")
        return 2

    print(f"Validated StandaloneBase {version}: {args.zip_file.resolve()} ({len(names)} files)")
    if args.dry_run:
        print("Dry run complete; no connection or changes made.")
        return 0

    password = args.password if args.password is not None else getpass.getpass("SSH password: ")
    radio = Radio(user, host, password, args.port, args.accept_new_host_key)
    stage = ""
    try:
        radio.connect()
        candidate = radio.run("mktemp -d /tmp/standalonebase-deploy.XXXXXX").strip()
        if not re.fullmatch(r"/tmp/standalonebase-deploy\.[A-Za-z0-9]+", candidate):
            raise RuntimeError(f"unsafe staging path returned by radio: {candidate!r}")
        stage = candidate
        radio.upload(args.zip_file.resolve(), stage, "package.zip")
        qstage = shlex.quote(stage)
        radio.run(f"mkdir {qstage}/files && unzip -q {qstage}/package.zip -d {qstage}/files")
        radio.run(
            f"chmod 755 {qstage}/files/sb-storage-helper && "
            f"{qstage}/files/sb-storage-helper verify-module {qstage}/files/sbubifs-authorized.ko && "
            f"{qstage}/files/sb-storage-helper verify-sbdata-image {qstage}/files/sbdata-empty.ubifs"
        )
        for destination in TARGETS:
            radio.run("mkdir -p " + shlex.quote(destination))
            for name in names:
                source = f"{stage}/files/{name}"
                target = f"{destination}/{name}"
                mode = "755" if name in EXECUTABLES else "644"
                radio.run(
                    f"cp {shlex.quote(source)} {shlex.quote(target + '.new')} && "
                    f"chmod {mode} {shlex.quote(target + '.new')} && "
                    f"mv {shlex.quote(target + '.new')} {shlex.quote(target)}"
                )
        radio.run(
            "mkdir -p /mnt/storage/standalonebase && "
            f"cp {qstage}/files/config.json /mnt/storage/standalonebase/config.json.new && "
            "chmod 644 /mnt/storage/standalonebase/config.json.new && "
            "mv /mnt/storage/standalonebase/config.json.new /mnt/storage/standalonebase/config.json"
        )
        # Patch before restart: SlimServer.lua is loaded early in SqueezePlay,
        # before the StandaloneBase applet itself is initialized.
        radio.run(
            "/usr/share/jive/applets/StandaloneBase/patch-squeezeplay-artwork.sh "
            "/usr/share/jive/jive/slim/SlimServer.lua"
        )
        if not args.no_restart:
            radio.run("/etc/init.d/squeezeplay restart")
        print(f"Deployed StandaloneBase {version} to {user}@{host}.")
        print("SqueezePlay restart skipped." if args.no_restart else "SqueezePlay restarted; no radio reboot is required.")
        return 0
    except Exception as exc:
        sys.stderr.write(f"deploy_dist: deployment failed: {exc}\n")
        return 1
    finally:
        if stage:
            try:
                radio.run("rm -rf " + shlex.quote(stage), attempts=2)
            except Exception as exc:
                sys.stderr.write(f"deploy_dist: warning: could not remove {stage}: {exc}\n")
        radio.client.close()


if __name__ == "__main__":
    raise SystemExit(main())
