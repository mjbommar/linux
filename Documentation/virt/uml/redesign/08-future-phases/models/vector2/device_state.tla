---- MODULE device_state ----
EXTENDS TLC

VARIABLE state

States == {
	"NEW",
	"CONFIGURED",
	"REGISTERED",
	"OPENING",
	"RUNNING",
	"QUIESCING",
	"DEAD"
}

Init == state = "NEW"

CanTransition(from, to) ==
	\/ from = "NEW" /\ to \in {"CONFIGURED", "DEAD"}
	\/ from = "CONFIGURED" /\ to \in {"REGISTERED", "DEAD"}
	\/ from = "REGISTERED" /\ to \in {"OPENING", "DEAD"}
	\/ from = "OPENING" /\ to \in {"RUNNING", "QUIESCING"}
	\/ from = "RUNNING" /\ to = "QUIESCING"
	\/ from = "QUIESCING" /\ to \in {"REGISTERED", "DEAD"}

Next ==
	\E to \in States:
		CanTransition(state, to) /\ state' = to

Spec == Init /\ [][Next]_<<state>>

TypeOK == state \in States
CanOpen == state = "REGISTERED"
CanXmit == state = "RUNNING"
Terminal == state = "DEAD"

====
