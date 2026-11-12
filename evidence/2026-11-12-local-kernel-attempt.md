# Local kernel build attempts

- Date: 2026-11-12
- Device inputs: UniXNU-kenzo revision `b9280aaa8c6f7fd2622bdc0b28aea8b9c35438a7`, SoC revision `894aea231e7ac7133b2947777b2dbc89bdd4c6aa`, UniXNU local revision `a294238d17aa155a2a6f873ef4f109decaf57ce1`.
- First command: `unixnu kernel -D ../UniXNU-kenzo --os ipados-17.7.11-21H461 -j 4` from WSL.
- First result: patch application stopped at the Sheng-specific `sheng_pmgr_run()` progress hunk in the generic `zzz-boot-progress.patch`; that function is absent from the generic XNU source. A local fix removed only those optional progress markers, and all 59 patches then applied.
- Second result: XNU bootstrap writes under `/mnt/c` failed when `installfile` atomically renamed generated headers (`Permission denied`). This is a host filesystem limitation; it did not indicate a compiler error.
- Third attempt: output was moved to the Linux filesystem at `/home/jason/unixnu-kenzo-out`. Source preparation and patching succeeded and XNU bootstrap started. The user then requested that compilation move to GitHub Actions, so this local process was interrupted before a build result or kernel artifact was produced.
- Conclusion: no local kernel build is claimed as successful. The committed GitHub Actions workflow now uses a macOS runner and applies the Sheng-only patch workaround in its isolated checkout.
