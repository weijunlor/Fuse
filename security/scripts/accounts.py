#!/usr/bin/env python3
"""Create private FUSE login credentials; never put passwords on argv/stdout."""
import argparse
import getpass
import hashlib
import os
from pathlib import Path
import re
import secrets
import tempfile

ROUNDS = 600_000


def private_write(path, contents):
    fd, temporary = tempfile.mkstemp(prefix=".account-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="ascii", newline="\n") as stream:
            stream.write(contents)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["demo", "add"])
    parser.add_argument("name", nargs="?")
    parser.add_argument("--directory", type=Path, default=Path(os.environ.get(
        "FUSE_AUTH_DIR", str(Path.home() / ".local/share/fuse/auth"))))
    args = parser.parse_args()
    directory = args.directory.expanduser()
    if str(directory.resolve()).startswith("/mnt/"):
        parser.error("use a Linux filesystem directory outside /mnt for private credentials")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    if directory.stat().st_uid != os.getuid():
        parser.error("credential directory must belong to you")
    directory.chmod(0o700)
    database = directory / "users.db"
    entries = {}
    if database.exists():
        for line in database.read_text(encoding="ascii").splitlines():
            if line and not line.startswith("#"):
                name, key = line.split(":")
                if not re.fullmatch(r"[A-Za-z0-9_-]{1,15}", name) or not re.fullmatch(r"[0-9a-fA-F]{64}", key) or name in entries:
                    parser.error("invalid existing database")
                entries[name] = key
    if args.command == "demo":
        names = [f"client-{i}" for i in range(1, 6)]
        if any(name in entries or (directory / f"{name}.pass").exists() for name in names):
            parser.error("demo accounts already exist; no credentials were changed")
        passwords = {name: secrets.token_urlsafe(24) for name in names}
    else:
        if args.name is None or not re.fullmatch(r"[A-Za-z0-9_-]{1,15}", args.name):
            parser.error("add requires a name of 1-15 letters, digits, _ or -")
        if args.name in entries:
            parser.error("account already exists; no credentials were changed")
        password = getpass.getpass("Password: ")
        if not password or len(password.encode("utf-8")) > 256 or any(ch in password for ch in "\r\n\0"):
            parser.error("password must contain 1-256 UTF-8 bytes and no line breaks/NUL")
        if password != getpass.getpass("Confirm password: "):
            parser.error("passwords do not match")
        passwords = {args.name: password}
    if any((directory / f"{name}.pass").exists() for name in passwords):
        parser.error("password file already exists; no credentials were changed")
    if len(entries) + len(passwords) > 32:
        parser.error("at most 32 accounts are supported")
    for name, password in passwords.items():
        entries[name] = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"),
                                          name.encode("ascii"), ROUNDS).hex()
        # Password files are for clients only. Copy each one privately to its client.
        fd = os.open(directory / f"{name}.pass", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(password + "\n")
    private_write(database, "# FUSE PBKDF2-HMAC-SHA256, 600000 rounds, account-name salt\n" +
                  "".join(f"{name}:{key}\n" for name, key in entries.items()))
    print(f"Created {len(passwords)} account(s) in {directory}; passwords were not printed.")


if __name__ == "__main__":
    main()
