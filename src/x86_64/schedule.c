#include <runtime.h>
#include <x86_64.h>
#include <tfs.h> // needed for unix.h
#include <unix.h> // some deps
#include <lock.h>
#include <apic.h>

//#define SCHED_DEBUG
#ifdef SCHED_DEBUG
#define sched_debug(x, ...) do {log_printf("SCHED", x, ##__VA_ARGS__);} while(0)
#else
#define sched_debug(x, ...)
#endif

// currently defined in x86_64.h
static char *state_strings_backing[] = {
    "not present",
    "idle",
    "kernel",
    "interrupt",
    "user",         
};

char **state_strings = state_strings_backing;
static int wakeup_vector;

/* could make a generic hook/register if more users... */
thunk unix_interrupt_checks;

queue runqueue;                 /* kernel space from ?*/
queue bhqueue;                  /* kernel from interrupt */
queue deferqueue;               /* kernel kernel (?) */
queue thread_queue;              /* kernel to user */
queue idle_cpu_queue;       

static timestamp runloop_timer_min;
static timestamp runloop_timer_max;

static thunk timer_update;

closure_function(0, 0, void, timer_update_internal)
{
    /* find timer interval from timer heap, bound by configurable min and max */
    timestamp timeout = MAX(MIN(timer_check(), runloop_timer_max), runloop_timer_min);
    runloop_timer(timeout);
}

void timer_schedule(void)
{
    enqueue(bhqueue, timer_update);
}

closure_function(0, 0, void, timer_interrupt_internal)
{
    timer_schedule();
}

thunk timer_interrupt;

#if 0
NOTRACE
void process_bhqueue()
{
    /* XXX - we're on bh frame & stack; re-enable ints here */
    thunk t;
    int defer_waiters = queue_length(deferqueue);
    while ((t = dequeue(bhqueue)) != INVALID_ADDRESS) {
        assert(t);
        apply(t);
    }

    /* only process deferred items that were queued prior to call -
       this allows bhqueue and deferqueue waiters to re-schedule for
       subsequent bh processing */
    while (defer_waiters > 0 && (t = dequeue(deferqueue)) != INVALID_ADDRESS) {
        assert(t);
        apply(t);
        defer_waiters--;
    }

    timer_update();

    /* XXX - and disable before frame pop */
    frame_pop();

    if (unix_interrupt_checks)
        apply(unix_interrupt_checks);

    current_cpu()->state = cpu_kernel; // ?? 
    interrupt_exit();
}
#endif

static u64 runloop_lock;
static u64 kernel_lock;

void kern_unlock()
{
    spin_unlock(&kernel_lock);
}


static void run_thunk(thunk t, int cpustate)
{
    sched_debug("%F\n", t);
    // as we are walking by, if there is work to be done and an idle cpu,
    // get it to wake up and examine the queue
    if ((queue_length(idle_cpu_queue) > 0 ) &&
        ((queue_length(bhqueue) > 0) ||
         (queue_length(runqueue) > 0) ||
         (queue_length(thread_queue) > 0))) {
        cpuinfo r = dequeue(idle_cpu_queue);
        if (r != INVALID_ADDRESS) 
            apic_ipi(r->id, 0, wakeup_vector);
    }
        
    current_cpu()->state = cpustate;
    spin_unlock(&runloop_lock);
    apply(t);
    // do we want to enforce this? i kinda just want to collapse
    // the stack and ensure that the thunk actually wanted to come back here
    //    halt("handler returned %d", cpustate);
}

// should we ever be in the user frame here? i .. guess so?
void runloop()
{
    thunk t;

    sched_debug("runloop %d %s b:%d r:%d t:%d ", current_cpu()->id, state_strings[current_cpu()->state], queue_length(bhqueue), queue_length(runqueue), queue_length(thread_queue));
    disable_interrupts();
    spin_lock(&runloop_lock);
    if (spin_try(&kernel_lock)) {
        //deferqueue scheduled under here
        if ((t = dequeue(bhqueue)) != INVALID_ADDRESS) {
            run_thunk(t, cpu_kernel);
//            rprintf("bh return\n");
//            kern_unlock();
//            runloop();
        }
        
        if ((t = dequeue(runqueue)) != INVALID_ADDRESS) {
            run_thunk(t, cpu_kernel);
            rprintf("rq return\n");
//            kern_unlock();
//            runloop();
        }
        kern_unlock();
//        spin_unlock(&kernel_lock);
    }

    if ((t = dequeue(thread_queue)) != INVALID_ADDRESS) 
        run_thunk(t, cpu_user);

    sched_debug("sleep\n");
    spin_unlock(&runloop_lock);
    kernel_sleep();
    halt("shouldn't be here");
}    


closure_function(0, 0, void, ipi_interrupt)
{
    cpuinfo ci = get_cpuinfo();
    ci->state = cpu_kernel;
}

void init_scheduler(heap h)
{
    unix_interrupt_checks = 0;
    runloop_lock = 0;
    kernel_lock = 0;
    timer_update = closure(h, timer_update_internal);
    timer_interrupt = closure(h, timer_interrupt_internal);
    runloop_timer_min = microseconds(RUNLOOP_TIMER_MIN_PERIOD_US);
    runloop_timer_max = microseconds(RUNLOOP_TIMER_MAX_PERIOD_US);
    wakeup_vector = allocate_interrupt();
    register_interrupt(wakeup_vector, closure(h, ipi_interrupt));    
    assert(wakeup_vector != INVALID_PHYSICAL);    
    idle_cpu_queue = allocate_queue(h, MAX_CPUS);
    /* scheduling queues init */
    runqueue = allocate_queue(h, 64);
    /* XXX bhqueue is large to accomodate vq completions; explore batch processing on vq side */
    bhqueue = allocate_queue(h, 2048);
    deferqueue = allocate_queue(h, 64);
    thread_queue = allocate_queue(h, 64);    
}


void kernel_sleep(void)
{
    // we're going to cover up this race by checking the state in the interrupt
    // handler...we shouldn't return here if we do get interrupted    
    cpuinfo ci = get_cpuinfo();
    ci->state = cpu_idle;
    spin_unlock(&kernel_lock); // ? 
    enqueue(idle_cpu_queue, pointer_from_u64((u64)ci->id));
    // wmb() ?  interrupt would probably enforce that
    __asm__("sti; hlt" ::: "memory");
    halt("return from kernel sleep");              
}

