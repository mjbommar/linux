# UML vector v2 model notes

These files are lightweight TLA+ starting points for the pure v2 helper
code in `arch/um/drivers/`.

- `device_state.tla` mirrors `um_vec2_dev_can_transition()`.
- `channel_state.tla` mirrors `um_vec2_chan_can_transition()`.
- `tx_ring.tla` models the TX ring counters used by
  `um_vec2_tx_ring_enqueue()`, `um_vec2_tx_ring_complete()`, and
  `um_vec2_tx_ring_reset()`.
- `rx_batch.tla` models the RX batch counters used by
  `um_vec2_rx_batch_prepare()`, `um_vec2_rx_batch_complete()`,
  `um_vec2_rx_batch_consume()`, and `um_vec2_rx_batch_reset()`.

The models intentionally omit Linux networking, `sk_buff` contents,
syscalls, NAPI, and IRQ delivery.  They describe the ownership counters
and lifecycle transitions that must remain true before those runtime
paths are wired in.

Example TLC use, assuming a local TLA+ tools jar:

```text
java -jar tla2tools.jar -deadlock device_state.tla
java -jar tla2tools.jar -deadlock channel_state.tla
```

The queue models are parameterized by `Depth`.  A small value such as
`Depth = 3` is enough to exercise empty, partial, full, and wraparound
states.
