# Adding API for parallel phase to task_arena to warm-up/retain/release worker threads

## Introduction

In oneTBB, there has never been an API that allows users to block worker threads within the arena.
This design choice was made to preserve the composability of the application.<br>
Before PR#1352, workers moved to the thread pool to sleep once there were no arenas with active
demand. However, PR#1352 introduced a delayed leave behavior to the library that
results in blocking threads for an _implementation-defined_ duration inside an arena
if there is no active demand arcoss all arenas. This change significantly
improved performance for various applications on high thread count systems.<br>
The main idea is that usually, after one parallel computation ends,
another will start after some time. The delayed leave behavior is a heuristic to utilize this,
covering most cases within _implementation-defined_ duration.

However, the new behavior is not the perfect match for all the scenarios:
* The heuristic of delayed leave is unsuitable for the tasks that are submitted
  in unpredictable pattern and/or duration.
* If oneTBB is used in composable scenarios it is not behaving as
  a good citizen consuming CPU resources.
  * For example, if an application builds a pipeline where oneTBB is used for one stage
    and OpenMP is used for a subsequent stage, there is a chance that oneTBB workers will
    interfere with OpenMP threads. This interference might result in slight oversubscription,
    which in turn might lead to underperformance.

So there are two related problems but with different resolutions:
* Completely disable new behavior for scenarios where the heuristic of delayed leave is unsuitable.
* Optimize library behavior so customers can benefit from the heuristic of delayed leave but
  make it possible to indicate that "it is the time to release threads".

## Proposal

Let's tackle these problems one by one.

### Completely disable new behavior

Let’s consider both “Delayed leave” and “Fast leave” as 2 different states in state machine.<br>
* The "Delayed leave" heuristic benefits most of the workloads. Therefore, this is the 
  default behavior for arena. 
* Workloads that has rather negative performance impact from the heuristic of delayed leave
  can create an arena in “Fast leave” state.

<img src="completely_disable_new_behavior.png" width=800>

There will be a question that we need to answer:
* Do we see any value if arena potentially can transition from one to another state?

To answer this question, the following scenarios should be considired:
* What if different types of workloads are mixed in one application?
* Different types of arenas can be used for different types of workloads.

### When threads should leave?

oneTBB itself can only guess when the ideal time to release threads from the arena is.
Therefore, it does the best effort to preserve and enhance performance without completely
messing composability guarantees (that is how delayed leave is implemented).

As we already discussed, there are cases where it does not work perfectly,
therefore customers that want to further optimize this
aspect of oneTBB behavior should be able to do it.

This problem can be considered from another angle. Essentially, if the user can indicate
where parallel computation ends, they can also indicate where it starts.

<img src="parallel_phase_introduction.png" width=800>

With this approach, the user not only releases threads when necessary but also specifies a
programmable block where worker threads should expected new work coming regularly
to the executing arena.

Let’s add new state to the existing state machine. To represent "Parallel Phase" state.

> **_NOTE:_** The "Fast leave" state is colored Grey just for simplicity of the chart.
              Let's assume that arena was created with the "Delayed leave". 
              The logic demonstrated below is applicable to the "Fast leave" as well.

<img src="parallel_phase_state_initial.png" width=800>

This state diagram leads to several questions. There are some of them:
* What if there are multiple Parallel Phases?
* If “End of Parallel Phase” leads back to “Delayed leave” how soon threads
  will be released from arena?
  * What if we indicated that threads should leave arena after the "Parallel Phase"?
  * What if we just indicated the end of the "Parallel Phase"?

The extended state machine aims to answer these questions.
* The first call to the “Start of PB” will transition into the “Parallel Phase” state.
* The last call to the “End of PB” will transition back to the “Delayed leave” state
  or into the "One-time Fast leave" if it is indicated that threads should leave sooner.
* Concurrent or nested calls to the “Start of PB” or the “End of PB”
  increment/decrement reference counter.

<img src="parallel_phase_state_final.png" width=800>

Let's consider the semantics that an API for explicit parallel phases can provide:
* Start of a parallel phase:
  * Indicates the point from which the scheduler can use a hint and keep threads in the arena
    for longer.
  * Serves as a warm-up hint to the scheduler:
    * Allows reducing computation start delays by initationg the wake-up of worker threads
      in advance.
* "Parallel phase" itself:
  * Scheduler can implement different policies to retain threads in the arena.
  * The semantics for retaining threads is a hint to the scheduler;
    thus, no real guarantee is provided. The scheduler can ignore the hint and
    move threads to another arena or to sleep if conditions are met.
* End of a parallel phase:
  * Indicates the point from which the scheduler may drop a hint and
    no longer retain threads in the arena.
  * Indicates that arena should enter the “One-time Fast leave” thus workers can leave sooner.
    * If work was submitted immediately after the end of the parallel phase,
      the default arena behavior with regard to "workers leave" policy is restored.
    * If the default "workers leave" policy was the "Fast leave", the result is NOP.


### Proposed API

Summary of API changes:

* Add enumeration class for the arena leave policy.
* Add the policy as the last parameter to the arena constructor and initializer
defaulted to "automatic".
* Add functions to start and end the parallel phase to the `task_arena` class
and the `this_task_arena` namespace.
* Add RAII class to map a parallel phase to a code scope.

```cpp
class task_arena {
    enum class leave_policy : /* unspecified type */ {
        automatic = /* unspecifed */,
        fast = /* unspecifed */,
        delayed = /* unspecifed */
    };

    task_arena(int max_concurrency = automatic, unsigned reserved_for_masters = 1,
               priority a_priority = priority::normal,
               leave_policy a_leave_policy = leave_policy::automatic);

    task_arena(const constraints& constraints_, unsigned reserved_for_masters = 1,
               priority a_priority = priority::normal,
               leave_policy a_leave_policy = leave_policy::automatic);

    void initialize(int max_concurrency, unsigned reserved_for_masters = 1,
                    priority a_priority = priority::normal,
                    leave_policy a_leave_policy = leave_policy::automatic);

    void initialize(constraints a_constraints, unsigned reserved_for_masters = 1,
                    priority a_priority = priority::normal,
                    leave_policy a_leave_policy = leave_policy::automatic);

    void start_parallel_phase();
    void end_parallel_phase(bool with_fast_leave = false);

    class scoped_parallel_phase {
        scoped_parallel_phase(task_arena& ta, bool with_fast_leave = false);
    };
};

namespace this_task_arena {
    void start_parallel_phase();
    void end_parallel_phase(bool with_fast_leave = false);
}
```

By the contract, users should indicate the end of _parallel phase_ for each
previous start of _parallel phase_.<br>
Let's introduce RAII scoped object that will help to manage the contract.

If the end of the parallel phase is not indicated by the user, it will be done automatically when
the last public reference is removed from the arena (i.e., task_arena is destroyed or a thread
is joined for an implicit arena). This ensures correctness is
preserved (threads will not be retained forever).

## Considerations

The alternative approaches were also considered.<br>
We can express this state machine as complete graph and provide low-level interface that
will give control over state transition.

<img src="alternative_proposal.png" width=600>

We considered this approach too low-level. Plus, it leaves a question: "How to manage concurrent changes of the state?".

The retaining of worker threads should be implemented with care because
it might introduce performance problems if:
* Threads cannot migrate to another arena because they are
  retained in the current arena.
* Compute resources are not homogeneous, e.g., the CPU is hybrid.
  Heavier involvement of less performant core types might result in artificial work
  imbalance in the arena.


## Open Questions in Design

Some open questions that remain:
* Are the suggested APIs sufficient?
* Are there additional use cases that should be considered that we missed in our analysis?
* Do we see any value if arena potentially can transition from one to another state?
  * What if different types of workloads are mixed in one application?
  * What if there concurrent calls to this API?