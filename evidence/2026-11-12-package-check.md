# Kenzo package preflight

- Date: 2026-11-12
- Command: `bash scripts/package.sh --check --os ipados-17.7.11-21H461`
- Device repository revision: `675602dbee807cf6b137d04413eb8c7930cee7e5`
- UniXNU revision: `5a00e20157f3975d155f13ced7593347f9f5bf73`
- SoC feed revision: `c9652aca1fe3c530ef79f5dae584dd85e61b1f8c`
- Result: passed on WSL/Linux. The command fetched the pinned SoC feed, generated the `kenzo_defconfig` configuration for `ipados-17.7.11-21H461`, extracted the profile's kernelcache, device tree and two trust caches, and resolved all 59 XNU patches.
- Updated input: `unixnu.lock` now records the MSM8956 feed revision.
- Scope: this was the packaging script's `--check` path only. It did not build XNU, UEFI firmware, payload or boot image, and did not boot a device.
- Environment note: UniXNU detected local changes in several submodules and skipped updating those submodules. Their working files were left untouched.
- Latest rerun after the SoC and device-tree updates: passed with device repository revision `b9280aaa8c6f7fd2622bdc0b28aea8b9c35438a7` and SoC feed revision `894aea231e7ac7133b2947777b2dbc89bdd4c6aa`; the result remained `59 patches, configuration kenzo_defconfig, OS ipados-17.7.11-21H461`.
