import os, subprocess, sys, re

CM = r"C:/Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

env = {}
for k, v in os.environ.items():
    env[k.lower()] = v          # case-insensitive unique
env = {k: v for k, v in env.items() if not k.endswith("_proxy")}

ver = "ae"
target = ""
if len(sys.argv) > 1:
    a = sys.argv[1].lower()
    if a in ("se", "ae"):
        ver = a
        if len(sys.argv) > 2:
            target = sys.argv[2]
    else:
        target = sys.argv[1]

build_dir = f"D:/dev/3SU/build_{ver}"
if not os.path.isdir(build_dir):
    print(f"build dir {build_dir} not found -- run build_all.py first")
    raise SystemExit(2)

cmd = [CM, "--build", build_dir, "--config", "Release", "--", "/nologo", "/v:m"]
if target:
    cmd = [CM, "--build", build_dir, "--config", "Release", "--target", target, "--", "/nologo", "/v:m"]
p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace")
out = (p.stdout or "") + (p.stderr or "")
for line in out.splitlines():
    if re.search(r"error|Error|错误|warning C|Build succeeded|生成成功|已成功|FAILED|失败", line):
        print(line)
print("EXIT", p.returncode)
