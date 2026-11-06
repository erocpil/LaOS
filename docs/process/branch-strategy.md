# Branch Strategy

## Branches

| Branch | Role | Description |
|--------|------|-------------|
| `x86_64` | **Trunk** | Shared-code development trunk and x86_64 implementation branch. All common kernel, driver, module, build and configuration changes land here first. Default `ARCH=x86_64`. |
| `main` | **Stable** | Stable releases. Fast-forward from `x86_64` after CI gate passes. |
| `arm64` | **Feature** | ARM64 porting branch. It is rebased onto `x86_64` and carries only ARM64-specific architecture code, configuration, tests, scripts and porting documentation. |

## Workflow

### Shared code change

Shared paths include common `kernel/` code, generic headers, module sources,
common build files and shared configuration.

```bash
git checkout x86_64
# ... make changes ...
make && make test-x86_64     # verify on x86_64
git commit -m "..."
git switch arm64
git rebase x86_64          # rebase to keep linear history
```

### ARM64-specific change

ARM64-only paths include `kernel/arch/aarch64/`, ARM64 task fixtures, test
scripts, porting documents and architecture-only module helpers.

```bash
git checkout arm64
# ... make changes ...
make ARCH=aarch64 && make test-arm64-limine
git commit -m "aarch64: ..."
```

### New architecture port (e.g., riscv64)

```bash
git checkout x86_64
git checkout -b riscv64
# ... add kernel/arch/riscv64/ ...
```

## Rationale

- **Single shared trunk (`x86_64`)** prevents common code from forking across architecture branches.
- **Architecture branches stay explicit**: ARM64-only implementation remains on `arm64`, while common interfaces remain identical to the trunk.
- **Shared-first rule** ensures no arch branch accumulates private copies of shared improvements that other arches can't see.

## History note

Prior to 2026-07 the `arm64` branch accumulated shared changes without going
through `x86_64`. As of 2026-07-25, `x86_64` is the merge-base and strict
ancestor of `arm64`; shared file contents are normalized, while ARM64-specific
commits remain ahead on the feature branch.
