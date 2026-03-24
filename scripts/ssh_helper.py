"""SSH helper: run commands on remote hosts with password authentication."""
import paramiko
import sys
import os

def run_ssh(host, user, password, command, timeout=120):
    """Run a command on a remote host and return stdout/stderr."""
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        client.connect(host, username=user, password=password, timeout=10)
        stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
        out = stdout.read().decode('utf-8', errors='replace')
        err = stderr.read().decode('utf-8', errors='replace')
        exit_code = stdout.channel.recv_exit_status()
        return exit_code, out, err
    finally:
        client.close()

def scp_upload(host, user, password, local_path, remote_path):
    """Upload a file via SFTP."""
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        client.connect(host, username=user, password=password, timeout=10)
        sftp = client.open_sftp()
        sftp.put(local_path, remote_path)
        sftp.close()
    finally:
        client.close()

def scp_download(host, user, password, remote_path, local_path):
    """Download a file via SFTP."""
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    try:
        client.connect(host, username=user, password=password, timeout=10)
        sftp = client.open_sftp()
        sftp.get(remote_path, local_path)
        sftp.close()
    finally:
        client.close()

if __name__ == "__main__":
    # Usage: python ssh_helper.py <host> <user> <password> <command>
    if len(sys.argv) < 5:
        print("Usage: ssh_helper.py <host> <user> <password> <command>")
        sys.exit(1)
    host, user, password = sys.argv[1], sys.argv[2], sys.argv[3]
    command = " ".join(sys.argv[4:])
    code, out, err = run_ssh(host, user, password, command)
    if out:
        print(out, end='')
    if err:
        print(err, end='', file=sys.stderr)
    sys.exit(code)
