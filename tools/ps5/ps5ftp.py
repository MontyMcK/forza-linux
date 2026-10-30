#!/usr/bin/env python3
"""FTP client for the console's ftpsrv (tools/ps5/ps5.env), standard library only.

  ps5ftp.py ls REMOTE
  ps5ftp.py put LOCAL REMOTE      file or folder; folders are uploaded recursively
  ps5ftp.py get REMOTE LOCAL      file or folder
  ps5ftp.py cat REMOTE

Rule 0 guards (CLAUDE.md): `put` refuses any local file under game_data/ or in a
folder that holds the game's exe, and only writes below PS5_TITLE_REMOTE.
Uploading the dump is tools/ps5/upload_dump.sh's job and goes through
`put --dump`, which writes only below PS5_DUMP_REMOTE.

ftpsrv quirks (v0.21.1): no NLST; MLSD ignores its path (CWD first); OPTS is
refused; SELF files are decrypted on download unless the SELF command turns
that off, so sizes of our fake-SELF eboot.bin would not match.
"""
import ftplib
import os
import posixpath
import sys

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))
GAME_EXE = "forza_durango_release_final.exe"


def load_env():
    env = {}
    path = os.path.join(REPO, "tools", "ps5", "ps5.env")
    if not os.path.exists(path):
        sys.exit(f"ps5ftp: {path} is missing (copy ps5.env.example)")
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if "=" in line:
            k, v = line.split("=", 1)
            env[k.strip()] = os.path.expandvars(v.strip())
    for k, v in os.environ.items():
        if k.startswith("PS5_"):
            env[k] = v
    return env


def looks_like_game_data(path):
    """True for anything under game_data/ or in a folder (or parent) holding the game exe."""
    real = os.path.realpath(path)
    if "game_data" in real.split(os.sep) or "game_data" in os.path.abspath(path).split(os.sep):
        return True
    for root in (os.path.join(REPO, "game_data"),
                 os.path.realpath(os.path.join(REPO, "game_data"))):
        if real == root or real.startswith(root + os.sep):
            return True
    d = real if os.path.isdir(real) else os.path.dirname(real)
    while d and d != os.path.dirname(d):
        if os.path.exists(os.path.join(d, GAME_EXE)):
            return True
        d = os.path.dirname(d)
    return False


def under(path, root):
    path, root = posixpath.normpath(path), posixpath.normpath(root)
    return path == root or path.startswith(root.rstrip("/") + "/")


class Console:
    def __init__(self, env):
        self.env = env
        self.ftp = ftplib.FTP()
        try:
            self.ftp.connect(env["PS5_IP"], int(env.get("PS5_FTP_PORT", 2121)), timeout=15)
        except OSError as e:
            sys.exit(f"ps5ftp: can't reach {env['PS5_IP']}:{env.get('PS5_FTP_PORT')} ({e}). "
                     "The console has probably rebooted: re-run the jailbreak.")
        self.ftp.login()
        self.ftp.voidcmd("TYPE I")
        # Raw bytes for SELF files (see module doc). SELF toggles; make sure it ends up off.
        try:
            r = self.ftp.sendcmd("SELF")
            if "enabled" in r.lower():
                self.ftp.sendcmd("SELF")
        except ftplib.error_perm:
            pass

    def entries(self, remote):
        """[(name, type, size)] of a remote folder, or None if it isn't one."""
        try:
            self.ftp.cwd(remote)
        except ftplib.error_perm:
            return None
        out = []
        for line in self._lines("MLSD"):
            facts, _, name = line.partition(" ")
            f = dict(x.split("=", 1) for x in facts.split(";") if "=" in x)
            t = f.get("type", "").lower()
            if name in (".", "..") or t in ("cdir", "pdir"):
                continue
            out.append((name, t, int(f.get("size", 0) or 0)))
        return out

    def _lines(self, cmd):
        lines = []
        self.ftp.retrlines(cmd, lines.append)
        return lines

    def size(self, remote):
        d, n = posixpath.split(remote)
        for name, t, size in self.entries(d) or []:
            if name == n and t == "file":
                return size
        return None

    def mkdirs(self, remote):
        cur = ""
        for part in remote.strip("/").split("/"):
            cur += "/" + part
            try:
                self.ftp.cwd(cur)
            except ftplib.error_perm:
                self.ftp.mkd(cur)

    def put_file(self, local, remote):
        self.mkdirs(posixpath.dirname(remote))
        tmp = posixpath.join(posixpath.dirname(remote), "." + posixpath.basename(remote) + ".upload")
        with open(local, "rb") as f:
            self.ftp.storbinary(f"STOR {tmp}", f, blocksize=1 << 20)
        if self.size(remote) is not None:
            self.ftp.delete(remote)
        self.ftp.rename(tmp, remote)
        want, got = os.path.getsize(local), self.size(remote)
        if want != got:
            sys.exit(f"ps5ftp: {remote}: uploaded {want} bytes, console has {got}")
        print(f"put {local} -> {remote} ({want} bytes)")

    def get_file(self, remote, local):
        os.makedirs(os.path.dirname(local) or ".", exist_ok=True)
        with open(local, "wb") as f:
            self.ftp.retrbinary(f"RETR {remote}", f.write, blocksize=1 << 20)
        print(f"get {remote} -> {local} ({os.path.getsize(local)} bytes)")

    def get(self, remote, local):
        ents = self.entries(remote)
        if ents is None:
            self.get_file(remote, local)
            return
        for name, t, _ in ents:
            if t == "dir":
                self.get(posixpath.join(remote, name), os.path.join(local, name))
            else:
                self.get_file(posixpath.join(remote, name), os.path.join(local, name))


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    env = load_env()
    cmd, args = argv[1], argv[2:]
    dump = "--dump" in args
    args = [a for a in args if a != "--dump"]
    c = Console(env)
    if cmd == "ls":
        ents = c.entries(args[0])
        if ents is None:
            sys.exit(f"ps5ftp: {args[0]}: not a folder")
        for name, t, size in sorted(ents):
            print(f"{'d' if t == 'dir' else '-'} {size:>14} {name}")
    elif cmd == "cat":
        c.ftp.retrbinary(f"RETR {args[0]}", sys.stdout.buffer.write)
    elif cmd == "get":
        c.get(args[0], args[1])
    elif cmd == "put":
        local, remote = args
        roots = [env["PS5_DUMP_REMOTE"]] if dump else [env["PS5_TITLE_REMOTE"]]
        if not any(under(remote, r) for r in roots):
            sys.exit(f"ps5ftp: refusing to write {remote}: only below {' or '.join(roots)}")
        files = []
        if os.path.isdir(local):
            for d, _, names in os.walk(local):
                for n in sorted(names):
                    p = os.path.join(d, n)
                    files.append((p, posixpath.join(remote, os.path.relpath(p, local).replace(os.sep, "/"))))
        else:
            files.append((local, remote))
        if not dump:
            bad = [p for p, _ in files if looks_like_game_data(p)]
            if bad:
                sys.exit("ps5ftp: refusing to upload game data (CLAUDE.md Rule 0):\n  " + "\n  ".join(bad))
        for p, r in files:
            c.put_file(p, r)
    else:
        sys.exit(__doc__)
    c.ftp.quit()


if __name__ == "__main__":
    main(sys.argv)
