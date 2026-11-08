#!/usr/bin/env python3
"""Install the host's SSH public key into the QNX VM's qnxuser account.

The quickstart image ships with password logins for its demo users. This
script logs in once with the documented default password (overridable with
QNX_PASSWORD) and appends ~/.ssh/id_ed25519.pub to ~/.ssh/authorized_keys on
the target, so later ssh/scp calls need no password.

Usage: scripts/setup-ssh-key.py [user]   (default user: qnxuser)
"""
import os
import pty
import select
import subprocess
import sys
import time

user = sys.argv[1] if len(sys.argv) > 1 else "qnxuser"
password = os.environ.get("QNX_PASSWORD", "qnxuser")
port = os.environ.get("QNX_SSH_PORT", "2222")
key = os.path.expanduser("~/.ssh/id_ed25519")

if not os.path.exists(key):
    subprocess.run(["ssh-keygen", "-t", "ed25519", "-N", "", "-f", key], check=True)
pub = open(key + ".pub").read().strip()

remote = (
    "mkdir -p ~/.ssh && chmod 700 ~/.ssh && "
    f"grep -qxF '{pub}' ~/.ssh/authorized_keys 2>/dev/null || echo '{pub}' >> ~/.ssh/authorized_keys; "
    "chmod 600 ~/.ssh/authorized_keys && echo KEY_INSTALLED"
)
cmd = [
    "ssh", "-p", port,
    "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
    "-o", "PubkeyAuthentication=no", "-o", "PreferredAuthentications=password,keyboard-interactive",
    f"{user}@127.0.0.1", remote,
]

pid, fd = pty.fork()
if pid == 0:
    os.execvp(cmd[0], cmd)

output = b""
sent = False
deadline = time.time() + 120
while time.time() < deadline:
    r, _, _ = select.select([fd], [], [], 1)
    if not r:
        continue
    try:
        chunk = os.read(fd, 1024)
    except OSError:
        break
    if not chunk:
        break
    output += chunk
    if not sent and b"assword" in output:
        os.write(fd, (password + "\n").encode())
        sent = True

text = output.decode(errors="replace")
if "KEY_INSTALLED" in text:
    print(f"SSH key installed for {user}. Try: ssh -p {port} {user}@127.0.0.1")
else:
    print("Key installation failed. ssh output:")
    print(text.replace(password, "***"))
    sys.exit(1)
