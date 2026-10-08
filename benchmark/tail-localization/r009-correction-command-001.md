# R009 最小纠错静态交付命令

Draft，仅供Reviewer只读核验和Leader申请额外Builder pure20审批。尚未授权执行；旧proc001 invalid永久保留。新37input seal替换历史broken test/helper执行条目；旧36seal只作历史保全。不执行任何compile/check/test/额外run。

```sh
/usr/bin/env TMPDIR=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TEMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp XDG_CACHE_HOME=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/localize_v12.py --role builder --run-id run-r008-proc-pure-002 --kind selfcheck --seconds 20 --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 -- /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/run_r009_proc_pure_v2.py --seal /home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache/r009-correction-seal-001/inputs.json --seal-sha256 1ccb2b765fdba42aea8ebe15a95162421859a84b7c3b5dc1abe2e7c092212622
```
