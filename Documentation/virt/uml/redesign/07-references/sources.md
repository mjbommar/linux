# Sources

URLs from the original research agents (2026-04-17) that
informed this plan. Grouped by topic.

## UML core

- https://docs.kernel.org/virt/uml/user_mode_linux_howto_v2.html
- https://en.wikipedia.org/wiki/User-mode_Linux
- https://lwn.net/Articles/142494/ (UML skas0 — separate kernel
  address space on stock hosts)
- https://github.com/torvalds/linux/blob/master/arch/um/Kconfig
- https://uml.devloop.org.uk/faq.html
- https://lwn.net/Articles/847192/ (PCI support for UML)
- https://user-mode-linux.sourceforge.net/old/debugging.html

## Recent UML work (linux-um list)

- Johannes Berg time-travel: https://netdevconf.info/0x14/pub/slides/8/UML%20Time%20Travel%20intro%20-%20netdev%200x14.pdf
- Berg seccomp work: https://www.mail-archive.com/linux-um@lists.infradead.org/msg06802.html
- Tiwei Bie SMP v4: https://www.mail-archive.com/linux-um@lists.infradead.org/msg07786.html
- Bie SMP cover letter: https://www.mail-archive.com/linux-um@lists.infradead.org/msg07786.html
- Berg execveat memfd: https://www.mail-archive.com/linux-um@lists.infradead.org/msg05036.html
- Anton Ivanov vector network removal: https://www.mail-archive.com/linux-um@lists.infradead.org/msg06711.html
- Hajime Tazaki nommu UML v14: https://www.mail-archive.com/linux-um@lists.infradead.org/msg08243.html
- Benjamin Berg nolibc port v3: https://lkml.org/lkml/2025/9/24/850
- LWN — UML SMP support: https://lwn.net/Articles/1033196/

## gVisor

- https://gvisor.dev/docs/architecture_guide/security/
- https://gvisor.dev/docs/architecture_guide/platforms/
- https://gvisor.dev/docs/architecture_guide/performance/
- https://gvisor.dev/blog/2023/04/28/systrap-release/
- https://opensource.googleblog.com/2023/06/optimizing-gvisor-filesystems-with-directfs.html
- https://github.com/google/gvisor/tree/master/pkg/sentry
- https://github.com/google/gvisor/blob/master/pkg/sentry/platform/kvm/machine_amd64.go
- https://github.com/google/gvisor/blob/master/pkg/sentry/syscalls/linux/linux64.go
- https://pages.cs.wisc.edu/~swift/papers/vee20-isolation.pdf (perf study)
- https://arxiv.org/html/2409.13139v1 (G-Fuzz)

## LKL

- https://github.com/lkl/linux
- https://lwn.net/Articles/662953/
- https://lwn.net/Articles/804177/ (Tazaki unification RFC)
- https://github.com/atrosinenko/kbdysch (LKL-based fuzzers)
- https://github.com/sslab-gatech/janus (Janus FS fuzzer)
- https://androidoffsec.withgoogle.com/slides/art_lkl_geekcon.pdf

## Firecracker

- https://firecracker-microvm.github.io/
- https://github.com/firecracker-microvm/firecracker
- https://github.com/firecracker-microvm/firecracker/blob/main/docs/design.md

## Cloud Hypervisor / Kata / crosvm

- https://github.com/cloud-hypervisor/cloud-hypervisor
- https://northflank.com/blog/guide-to-cloud-hypervisor
- https://northflank.com/blog/kata-containers-vs-firecracker-vs-gvisor
- https://crosvm.dev/book/architecture/overview.html
- https://crosvm.dev/book/appendix/sandboxing.html
- https://crosvm.dev/book/appendix/seccomp.html

## Unikraft / unikernels

- https://github.com/unikraft/unikraft
- https://www.usenix.org/conference/atc21/presentation/kuo
- https://tarides.com/blog/2025-11-13-announcing-unikraft-support-for-mirageos-unikernels/
- https://tarides.com/blog/2025-02-06-mirageos-on-ocaml-5/

## seL4 / Bao / Jailhouse

- https://sandro2pinto.github.io/files/ew2020-bao.pdf
- https://sel4.systems/Summit/2025/abstracts2025.html
- https://arxiv.org/pdf/2303.11186 (static partitioning HVs)

## Nabla / minimized syscalls

- https://nabla-containers.github.io/
- https://github.com/nabla-containers/runnc

## Wasm / WASI

- https://eunomia.dev/blog/2025/02/16/wasi-and-the-webassembly-component-model-current-status/
- https://www.phoronix.com/news/Linux-Kernel-WebAssembly
- (EuroSys 2025) https://dl.acm.org/doi/10.1145/3689031.3717470

## eBPF

- https://ebpf.io/
- https://eunomia.dev/blog/2025/02/12/ebpf-ecosystem-progress-in-20242025-a-technical-deep-dive/
- https://eunomia.dev/blogs/bpftime/

## BULKHEAD / intra-kernel isolation

- https://www.ndss-symposium.org/wp-content/uploads/2025-s328-paper.pdf
- https://arxiv.org/html/2409.09606v1

## Kernel observability / sanitizers

- https://docs.kernel.org/dev-tools/kasan.html
- https://docs.kernel.org/dev-tools/kcsan.html
- https://docs.kernel.org/dev-tools/kfence.html
- https://docs.kernel.org/dev-tools/kmsan.html
- https://docs.kernel.org/dev-tools/kcov.html
- https://lore.kernel.org/lkml/20220525111756.GA15955@axis.com/T/ (UML+KASAN v4)
- https://patchwork.ozlabs.org/patch/1244590/ (UML+KASAN)
- https://github.com/iovisor/bcc/blob/master/docs/kernel-versions.md (BPF features)

## Syzkaller

- https://github.com/google/syzkaller/issues/1288 (UML investigation, open since 2019-07-15)
- https://github.com/google/syzkaller/blob/master/docs/syzbot.md
- https://github.com/google/syzkaller/blob/master/docs/gvisor/README.md
- https://pkg.go.dev/github.com/google/syzkaller/sys/targets

## WSL

- https://en.wikipedia.org/wiki/Windows_Subsystem_for_Linux
- https://deepwiki.com/MicrosoftDocs/WSL/1.1-wsl1-vs-wsl2-architecture

## Coding-assistant policy (April 2026 merge)

- https://docs.kernel.org/process/coding-assistants.html
- https://lwn.net/Articles/1031473/
- https://lwn.net/Articles/1032612/

## b4 + lei (development tooling)

- https://b4.docs.kernel.org/en/latest/contributor/prep.html
- https://b4.docs.kernel.org/en/latest/config.html
- https://lwn.net/Articles/1064097/ (b4 v0.15.0 hooks)
- https://people.kernel.org/monsieuricon/lore-lei-part-1-getting-started

## Original research agent reports

Archived in:
- `/tmp/claude-1000/-nas4-data-workspace-infosec-linux-security-paper/e4e4b63b-c96a-41e0-bccb-6c4fc069c8d7/tasks/a55f33855ce10c4ea.output` (UML architecture)
- `.../a737124a9db5fb523.output` (b4 tooling research)
- `.../a215e6f002b975bfc.output` (AI-assistance trailer policy)
- `.../ada2eff1ba9fa7c10.output` (gVisor architecture)
- `.../a1cfd98507b65ac04.output` (alternatives)
- `.../a850d022eb2c5d719.output` (instrumentation)
- `.../a430ef45fdf41fb80.output` (perf/networking/SMP)
