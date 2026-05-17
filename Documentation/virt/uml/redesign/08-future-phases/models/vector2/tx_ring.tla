---- MODULE tx_ring ----
EXTENDS Naturals, TLC

CONSTANT Depth

VARIABLES head, tail, count

Init ==
	/\ Depth \in 1..4096
	/\ head = 0
	/\ tail = 0
	/\ count = 0

Enqueue ==
	/\ count < Depth
	/\ head' = head
	/\ tail' = (tail + 1) % Depth
	/\ count' = count + 1

Complete(n) ==
	/\ n \in 0..count
	/\ head' = (head + n) % Depth
	/\ tail' = tail
	/\ count' = count - n

Reset ==
	/\ head' = 0
	/\ tail' = 0
	/\ count' = 0

Next ==
	\/ Enqueue
	\/ \E n \in 0..count: Complete(n)
	\/ Reset

Spec == Init /\ [][Next]_<<head, tail, count>>

TypeOK ==
	/\ head \in 0..(Depth - 1)
	/\ tail \in 0..(Depth - 1)
	/\ count \in 0..Depth

Empty == count = 0
Full == count = Depth
OccupancyBound == count <= Depth

====
