# R009 corrected pure command 002

Status: Draft — 未批准执行。R010仅额外Builder pure002单次20秒（含7秒cleanup），本材料仅静态准备，未执行compile或测试。

37输入seal：`/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache/r009-correction-seal-002/inputs.json`，SHA256 `c9db93d5f51ca5466709a631978041ef07c50f419cb6879eac30cb0608f5892c`。已保全旧v2/001；新v3计划20项，增加error.target_pid=9003、child.pid=9002并断言9003的区分反例；v12行为不变。

```bash
/usr/bin/env TMPDIR=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp TEMP=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/tmp XDG_CACHE_HOME=/home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache PYTHONDONTWRITEBYTECODE=1 /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/localize_v12.py --role builder --run-id run-r008-proc-pure-002 --kind selfcheck --seconds 20 --static-inventory-sha256 8199f58a268884125ce50c454821fc6318533e50824817e2306726273c782b61 -- /usr/bin/python3 /home/power/projects/HP-HTTP-Server/benchmark/tail-localization/run_r009_proc_pure_v3.py --seal /home/power/projects/HP-HTTP-Server/.cache/v0.5.1-s4/builder/cache/r009-correction-seal-002/inputs.json --seal-sha256 c9db93d5f51ca5466709a631978041ef07c50f419cb6879eac30cb0608f5892c
```
