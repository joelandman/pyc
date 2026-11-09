# email/mbox parse of a large, deterministic mailbox.
#
# Perf workload (STATUS.md, real-world performance). The mailbox is
# built in memory from fixed patterns (no file, no randomness, no
# clock), written to a temp file because mailbox.mbox takes a path, and
# the harness runs each workload in its own sandboxed cwd/TMPDIR. The
# email parser is pure Python and call-dense -- the class where native
# dispatch is expected to win.
import mailbox
import os
import tempfile

MESSAGES = 20000
BODY_LINES = 12

def make_mbox():
    out = []
    for i in range(MESSAGES):
        out.append(
            f"From - Mon Nov  8 00:{i % 60:02d}:00 2026\n"
            f"From: user{i % 25}@example.com\n"
            f"To: list{i % 7}@example.com\n"
            f"Subject: deterministic message {i}\n\n")
        out.extend("Body line with some words to parse.\n" for _ in range(BODY_LINES))
        out.append("\n")
    return "".join(out)

def main():
    data = make_mbox()
    fd, path = tempfile.mkstemp(suffix=".mbox")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(data)
        mb = mailbox.mbox(path)
        n = 0
        subject = body = ""
        for msg in mb:
            n += 1
            subject = msg["Subject"]
            body = msg.get_payload()
        print(n, len(data), bool(subject), bool(body))
    finally:
        os.unlink(path)

main()
