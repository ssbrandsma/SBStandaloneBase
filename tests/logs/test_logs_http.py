#!/usr/bin/env python3
"""Host integration coverage for the stateless system-log API."""

import json
import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request


def request(base, path, cookie=None, method="GET", body=None, headers=None):
    data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
    request_headers = dict(headers or {})
    if cookie:
        request_headers["Cookie"] = cookie
    if data is not None:
        request_headers.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(base + path, data=data, method=method, headers=request_headers)
    try:
        with urllib.request.urlopen(req, timeout=3) as response:
            return response.status, response.read(), dict(response.headers)
    except urllib.error.HTTPError as error:
        return error.code, error.read(), dict(error.headers)


def query(base, source, cookie, boot="", cursor="", lines=200):
    values = {"lines": str(lines)}
    if boot:
        values["boot_id"] = boot
    if cursor:
        values["cursor"] = cursor
    status, body, _ = request(base, "/api/logs/" + source + "?" + urllib.parse.urlencode(values), cookie)
    assert status == 200, (status, body)
    return json.loads(body)


def start_server(executable, web_root, root, messages, dmesg, boot_file, port):
    (root / "config").mkdir(exist_ok=True)
    environment = os.environ | {
        "SB_LOG_MESSAGES_PATH": str(messages),
        "SB_LOG_DMESG_PATH": str(dmesg),
        "SB_LOG_BOOT_ID_PATH": str(boot_file),
    }
    process = subprocess.Popen(
        [executable, "--port", str(port), "--web-root", web_root, "--config-dir", str(root / "config")],
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    for _ in range(80):
        try:
            socket.create_connection(("127.0.0.1", port), 0.1).close()
            break
        except OSError:
            time.sleep(0.05)
    else:
        raise RuntimeError("webserver did not start")
    return process


def pair(base, config_root):
    capability = (config_root / "remote.capability").read_text().strip()
    status, body, _ = request(
        base,
        "/api/internal/web-access/start",
        method="POST",
        body=b"",
        headers={"X-Remote-Capability": capability},
    )
    assert status == 200
    pin = json.loads(body)["pin"]
    status, _, headers = request(base, "/api/auth/pair", method="POST", body={"pin": pin})
    assert status == 200
    return headers["Set-Cookie"].split(";", 1)[0]


def main():
    executable, web_root = sys.argv[1:3]
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        messages = root / "messages"
        dmesg = root / "dmesg"
        boot_file = root / "boot_id"
        boot_file.write_text("boot-a\n")
        messages.write_text("".join(f"message-{index:03d}\n" for index in range(1, 251)))
        dmesg.write_text("".join(f"kernel-{index:03d}\n" for index in range(1, 11)))
        port = 20500 + os.getpid() % 1000
        base = f"http://127.0.0.1:{port}"
        process = start_server(executable, web_root, root, messages, dmesg, boot_file, port)
        try:
            cookie = pair(base, root / "config")
            assert request(base, "/api/logs/messages")[0] == 401

            initial = query(base, "messages", cookie)
            assert initial["boot_id"] == "boot-a"
            assert initial["lines"][0] == "message-051" and initial["lines"][-1] == "message-250"
            assert len(initial["lines"]) == 200 and not initial["reset"]
            second_client = query(base, "messages", cookie, lines=3)
            assert second_client["lines"] == ["message-248", "message-249", "message-250"]
            unchanged = query(base, "messages", cookie, initial["boot_id"], initial["next_cursor"])
            assert unchanged["lines"] == [] and unchanged["next_cursor"] == initial["next_cursor"]

            with messages.open("a") as stream:
                stream.write("partial")
            partial = query(base, "messages", cookie, unchanged["boot_id"], unchanged["next_cursor"])
            assert partial["lines"] == [] and not partial["more"]
            with messages.open("a") as stream:
                stream.write(" complete\nidentical\nidentical\n")
            completed = query(base, "messages", cookie, partial["boot_id"], partial["next_cursor"])
            assert completed["lines"] == ["partial complete", "identical", "identical"]

            cursor = completed["next_cursor"]
            with messages.open("a") as stream:
                for index in range(700):
                    stream.write(f"page-{index:03d}\n")
            page_one = query(base, "messages", cookie, "boot-a", cursor)
            page_two = query(base, "messages", cookie, "boot-a", page_one["next_cursor"])
            assert len(page_one["lines"]) == 500 and page_one["more"]
            assert len(page_two["lines"]) == 200 and not page_two["more"]
            assert page_one["lines"][0] == "page-000" and page_two["lines"][-1] == "page-699"

            old_cursor = page_two["next_cursor"]
            messages.write_text("after-truncate-1\nafter-truncate-2\n")
            truncated = query(base, "messages", cookie, "boot-a", old_cursor)
            assert truncated["reset"] and truncated["reset_reason"] == "file_truncated"
            assert truncated["lines"] == ["after-truncate-1", "after-truncate-2"]

            replacement = root / "messages.new"
            replacement.write_text("replacement\n" * 100)
            os.replace(replacement, messages)
            replaced = query(base, "messages", cookie, "boot-a", truncated["next_cursor"])
            assert replaced["reset"] and replaced["reset_reason"] == "file_replaced"

            kernel = query(base, "dmesg", cookie, lines=3)
            assert kernel["lines"] == ["kernel-008", "kernel-009", "kernel-010"]
            with dmesg.open("a") as stream:
                stream.write("kernel-010\nkernel-011\n")
            kernel_new = query(base, "dmesg", cookie, kernel["boot_id"], kernel["next_cursor"])
            assert kernel_new["lines"] == ["kernel-010", "kernel-011"]
            invalid = query(base, "dmesg", cookie, "boot-a", "invalid", lines=2)
            assert invalid["reset"] and invalid["reset_reason"] == "invalid_cursor"
            dmesg.write_text("wrapped-a\nwrapped-b\n")
            wrapped = query(base, "dmesg", cookie, "boot-a", kernel_new["next_cursor"])
            assert wrapped["reset"] and wrapped["reset_reason"] == "lost_overlap"

            boot_file.write_text("boot-b\n")
            messages.write_text("new-boot-message\n")
            dmesg.write_text("new-boot-kernel\n")
            reboot_messages = query(base, "messages", cookie, "boot-a", replaced["next_cursor"])
            reboot_kernel = query(base, "dmesg", cookie, "boot-a", wrapped["next_cursor"])
            assert reboot_messages["reset_reason"] == "reboot" and reboot_messages["lines"] == ["new-boot-message"]
            assert reboot_kernel["reset_reason"] == "reboot" and reboot_kernel["lines"] == ["new-boot-kernel"]

            process.terminate()
            process.wait(timeout=3)
            process = start_server(executable, web_root, root, messages, dmesg, boot_file, port)
            cookie = pair(base, root / "config")
            restarted = query(base, "messages", cookie, "boot-b", reboot_messages["next_cursor"])
            assert not restarted["reset"] and restarted["lines"] == []

            script = pathlib.Path(web_root, "js", "logs.js").read_text()
            assert "RETAINED_LINES=20000" in script and "new Blob" in script and "POLL_MS=2000" in script
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=3)
    print("system-log-viewer-tests-ok")


if __name__ == "__main__":
    main()
