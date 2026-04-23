"""SSH helper: run commands on remote hosts with password authentication."""
import paramiko
import sys
import os


class SSHSession:
    """Reusable SSH session — use as a context manager to share one connection."""

    def __init__(self, host, user, password, connect_timeout=10):
        self.host = host
        self.user = user
        self.password = password
        self.connect_timeout = connect_timeout
        self._client = None

    def __enter__(self):
        self._client = paramiko.SSHClient()
        self._client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        self._client.connect(
            self.host, username=self.user, password=self.password,
            timeout=self.connect_timeout,
        )
        return self

    def __exit__(self, *_):
        if self._client:
            self._client.close()
            self._client = None

    def run(self, command, timeout=120, stream=False):
        """Run a command. Returns (exit_code, stdout, stderr).

        If stream=True, stdout is printed line-by-line in real time and
        returned as an empty string.
        """
        _, stdout, stderr = self._client.exec_command(command, timeout=timeout)
        if stream:
            for line in iter(stdout.readline, ""):
                print(line, end="", flush=True)
            err = stderr.read().decode("utf-8", errors="replace")
            return stdout.channel.recv_exit_status(), "", err
        out = stdout.read().decode("utf-8", errors="replace")
        err = stderr.read().decode("utf-8", errors="replace")
        return stdout.channel.recv_exit_status(), out, err

    def upload(self, local_path, remote_path, callback=None):
        """Upload a file via SFTP. Optional callback(transferred, total)."""
        with self._client.open_sftp() as sftp:
            sftp.put(local_path, remote_path, callback=callback)

    def download(self, remote_path, local_path, callback=None):
        """Download a file via SFTP. Optional callback(transferred, total)."""
        with self._client.open_sftp() as sftp:
            sftp.get(remote_path, local_path, callback=callback)


# ---------------------------------------------------------------------------
# Convenience one-shot helpers (backward-compatible)
# ---------------------------------------------------------------------------

def run_ssh(host, user, password, command, timeout=120):
    """Run a command on a remote host and return (exit_code, stdout, stderr)."""
    with SSHSession(host, user, password) as s:
        return s.run(command, timeout=timeout)


def scp_upload(host, user, password, local_path, remote_path):
    """Upload a file via SFTP."""
    with SSHSession(host, user, password) as s:
        s.upload(local_path, remote_path)


def scp_download(host, user, password, remote_path, local_path):
    """Download a file via SFTP."""
    with SSHSession(host, user, password) as s:
        s.download(remote_path, local_path)


if __name__ == "__main__":
    # Usage: python ssh_helper.py <host> <user> <password> <command...>
    if len(sys.argv) < 5:
        print("Usage: ssh_helper.py <host> <user> <password> <command>")
        sys.exit(1)
    _host, _user, _pw = sys.argv[1], sys.argv[2], sys.argv[3]
    _cmd = " ".join(sys.argv[4:])
    _code, _out, _err = run_ssh(_host, _user, _pw, _cmd)
    if _out:
        print(_out, end="")
    if _err:
        print(_err, end="", file=sys.stderr)
    sys.exit(_code)
