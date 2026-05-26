---- MODULE rx_batch ----
EXTENDS Naturals, TLC

CONSTANT Depth

VARIABLES prepared, filled

Init ==
	/\ Depth \in 1..4096
	/\ prepared = 0
	/\ filled = 0

Prepare(n) ==
	/\ n \in 0..Depth
	/\ prepared = 0
	/\ filled = 0
	/\ prepared' = n
	/\ filled' = 0

Receive(n) ==
	/\ n \in 0..prepared
	/\ prepared' = 0
	/\ filled' = n

Consume(n) ==
	/\ n \in 0..filled
	/\ prepared' = prepared
	/\ filled' = filled - n

Reset ==
	/\ prepared' = 0
	/\ filled' = 0

Next ==
	\/ \E n \in 0..Depth: Prepare(n)
	\/ \E n \in 0..prepared: Receive(n)
	\/ \E n \in 0..filled: Consume(n)
	\/ Reset

Spec == Init /\ [][Next]_<<prepared, filled>>

TypeOK ==
	/\ prepared \in 0..Depth
	/\ filled \in 0..Depth

Idle == prepared = 0 /\ filled = 0
OwnershipBound == prepared + filled <= Depth

====
