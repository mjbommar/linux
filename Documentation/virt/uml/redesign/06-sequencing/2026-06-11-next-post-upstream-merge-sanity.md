# UML Next Post-Upstream-Merge Sanity

Date: 2026-06-11

Branch: `next`

Post-merge commit: `b1c2aad96149`

Merged upstream: `torvalds/master` at `2b414a95b8f7`

Status: PASS for the focused post-merge sanity checks below.

## Scope

This note records the validation done after merging current Linus
`torvalds/master` into `next`. It does not replace the natural 7200-second
seccomp/vector2 Tier 3 soak recorded in
`2026-06-11-vector2-seccomp-natural-soak.md`; that long-soak artifact remains
the long-run evidence from commit `899e80995800`.

The post-merge goal was to prove that the newly merged HEAD still builds as a
UML kernel and still passes the focused vector2 KUnit surface after the
upstream networking refresh.

## Commands

```sh
git fetch torvalds master
git merge --no-ff torvalds/master -m "Merge torvalds/master into next"
make ARCH=um olddefconfig
make ARCH=um -j"$(nproc)" linux
timeout 180 ./linux mem=256M \
  kunit.filter_glob='um_vector2_*' \
  kunit_shutdown=halt con=null con0=fd:0,fd:1 panic=-1 \
  > /home/mjbommar/projects/personal/.tmp/uml-vector2/post-merge-b1c2aad96149/vector2-kunit.log 2>&1
```

`olddefconfig` reported no `.config` change. The UML build completed and
linked `linux`.

## Result

The saved KUnit log reported:

```text
um: backend = seccomp (contract v2)
Linux version 7.1.0-rc7-00350-gb1c2aad96149
```

Vector2 KUnit totals:

| Suite | Result |
| --- | --- |
| `um_vector2_config` | 12 pass, 0 fail, 0 skip |
| `um_vector2_queue` | 9 pass, 0 fail, 0 skip |
| `um_vector2_transport` | 8 pass, 0 fail, 0 skip |
| `um_vector2_fake_host` | 10 pass, 0 fail, 0 skip |
| `um_vector2_model` | 7 pass, 0 fail, 0 skip |
| `um_vector2_cmdline` | 5 pass, 0 fail, 0 skip |
| `um_vector2_netdev` | 18 pass, 0 fail, 0 skip |
| `um_vector2_ethtool` | 6 pass, 0 fail, 0 skip |
| `um_vector2_host_fd` | 14 pass, 0 fail, 0 skip |
| `um_vector2_host_tap` | 7 pass, 0 fail, 2 skip |

Total: 96 pass, 0 fail, 2 skip. The two skips are the expected trusted
in-process TAP host-open cases.

A focused scan of the saved log for `not ok`, panic, BUG, WARNING, KCSAN,
data-race, and FAILED markers returned no matches.

## Interpretation

The post-merge `next` head is up to date with Linus through
`torvalds/master` `2b414a95b8f7`, rebuilds as UML, and passes the focused
vector2 KUnit suite.

The natural seccomp/vector2 Tier 3 long-soak gate remains observed 100% pass
evidence for its tested tree. A fresh long-soak rerun on `b1c2aad96149` would
be needed only if the publication bar requires natural long-run evidence
exactly at the post-merge HEAD.
