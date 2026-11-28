# corpus case — ground truth is CPython at run time (CHARTER I5).
# stress: dict unpacking in calls, mixed positional and **kwargs.
def config(host, port, timeout=1.0, **opts):
    return (host, port, timeout, sorted(opts))


print(config("h", 80))
print(config("h", 80, timeout=2, retry=3, region="us"))
base = {"timeout": 5, "region": "eu"}
print(config("h", 443, **base))
