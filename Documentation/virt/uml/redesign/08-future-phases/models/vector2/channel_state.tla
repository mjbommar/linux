---- MODULE channel_state ----
EXTENDS TLC

VARIABLE state

States == {
	"UNINIT",
	"ALLOCATED",
	"FD_ATTACHED",
	"IRQ_ATTACHED",
	"NAPI_ENABLED",
	"ACTIVE",
	"QUIESCING",
	"CLOSED"
}

Init == state = "UNINIT"

CanTransition(from, to) ==
	\/ from = "UNINIT" /\ to \in {"ALLOCATED", "CLOSED"}
	\/ from = "ALLOCATED" /\ to \in {"FD_ATTACHED", "QUIESCING", "CLOSED"}
	\/ from = "FD_ATTACHED" /\ to \in {"IRQ_ATTACHED", "QUIESCING"}
	\/ from = "IRQ_ATTACHED" /\ to \in {"NAPI_ENABLED", "QUIESCING"}
	\/ from = "NAPI_ENABLED" /\ to \in {"ACTIVE", "QUIESCING"}
	\/ from = "ACTIVE" /\ to = "QUIESCING"
	\/ from = "QUIESCING" /\ to = "CLOSED"

Next ==
	\E to \in States:
		CanTransition(state, to) /\ state' = to

Spec == Init /\ [][Next]_<<state>>

TypeOK == state \in States
Active == state = "ACTIVE"
Terminal == state = "CLOSED"

====
