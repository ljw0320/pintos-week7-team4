/* The main thread acquires a lock.  Then it creates a
   higher-priority thread that blocks acquiring the lock, causing
   it to donate their priorities to the main thread.  The main
   thread attempts to lower its priority, which should not take
   effect until the donation is released. */

/*   
     1. TC 이해

     기대 동작: 
          lock을 들고 있는 실행중인 스레드는 우선순위 높은 스레드가 선점
          에게 (우선순위가 자신보다 낮다면) 우선순위를 기부하고 기부 받기 전의 우선순위를 기억.
          lock이 해제되면 기존 우선순위 복구.
     실행 흐름:      
          1. lock acquire : main. main p = PRI_DEFAULT.
          2. acquire create. lock holder : main. main p = PRI_DEFAULT + 10. (priorty donated from acquire)
          3. priorty donate: acquire -> main. lock holder : main. main p = PRI_DEFAULT + 10
          4. ouput msg 1 
          5. main p = PRI_DEFAULT - 10
          6. priorty donate: acquire -> main. lock holder : main. main p = PRI_DEFAULT + 10
          7. lock realse : main. 
          8. main p = PRI_DEFAULT - 10 
          9. lock acquire : acquire
          10. "acquire: got the lock"
          11. lock realse : acquire
          12. "acquire: done"

     2. 기존 코드

     관련 코드: 
          thread_get_priority ()   // thread.c
          thread_create ()         // thread.c
          thread_set_priority ()   // thread.c

          lock_acquire ()          // synch.c
          lock_release ()          // synch.c

     현재 동작과 한계: 
          - priority donation 미구현
          - 기존 priority 저장 기능 필요

     3. 구현 아이디어
          핵심 :
               # lock이 필요한데 이미 다른 스레드가 점유 중이면, 그 스레드에게 우선순위 donation 후 리스트(waiters)에 넣고 block 
               # lock이 해제 되면, waiter list 에서 가장 높은 우선순위의 스레드가 실행됨. 
               # lock이 해제된 스레드는 다른 lock이 있는지 확인
                    - 있으면 lock waiter에서 가장
                     높은 우선순위와 기존 우선순위를 비교해서 가져옴
                    - 없으면 기존 우선순위를 우선순위로 가져옴
          접근 방식: 
               # 우선순위가 높은 ready 스레드는 lock이 필요한지 모르기 때문에, lock 들고 있는 스레드가 실행 중이라도 컨텍스트 스위칭은 일어나야 함
               # 다른 스레드가 선점 후, 실행 도중 lock이 필요하면 lock을 요청하면서 우선 순위 도네이션 
               # 요청한 스레드를 lock waiters 에 넣고 block               

               - 기존 생각  
               => lock이 기부 받은 우선순위를 소유 => 단일 스레드가 다수 lock 점유해도 문제 x int priority_donated;

               - 변경된 점               
               => thread가 기존 우선순위(int priority_org)와 실효 우선순위(priority)를 보유
               => *'기존 우선순위(priority_org)'는 init할 때 설정된 우선순위로 두고, 오직 thread_set_priority로 변경할 때만 변함*

               - lock holder는 donation 받은 우선순위와 본인의 우선순위 중 가장 높은 우선 순위를 보유(lock->sema->waiter 중 최댓값)
               => 각 thread는 보유중인 lock들에 대한 컨테이너가 존재

               - 기존 thread_set_priority :
                    thread_current ()->priority = new_priority;

               => 추가 or 수정 :
                    thread_current ()->priority_org = new_priority;                    

               - 기존 acquire : 
                    1. lock_held_by_current_thread로 해당 lock을 현재 스레드가 갖고 있지 않을 때만 실행                    
                    2. sema_down 호출(인터럽트 꺼줌)
                    3. 이미 다른 스레드가 lock을 사용 중이라면 waiter 리스트에 스레드 넣고 block 호출                     
                    
               => 추가 or 수정 :
                    - block 전 priority donate(sema->waiter에 넣은 후)

               - 기존 release : 
                    1. lock_held_by_current_thread로 해당 lock을 현재 스레드가 갖고 있을 때만 실행                    
                    2. lock->holder = NULL
                    3. sema_up 호출(인터럽트 꺼줌)
                    4. ...

               => 추가 or 수정 :
                    - unblock 전 priority donate(sema->waiter에 넣은 후)

               - donation 함수 : lock을 들고 있는 스레드가 가진 가장 높은 우선 순위 설정
                    - 홀더가 갖고 있는 우선순위와 비교
                         - 기존 우선순위 보다 작다면 바로 return                    
                    - 인자로 도네이션하는 스레드와 대상 lock을 받음
                    - caller : acquire 및 release (추가로 있는지는 고려 필요)               

               thread_donate_priority(lock *lock_) {
                    thread *receiver = lock_->holder;                    
                    lock_->semaphore->waiter
                     
               }               

          고려 사항:


     4. 함께 논의할 것
          다른 TC의 구현과 겹치거나 영향을 주는 부분
          확신이 없는 설계 판단, 비교할 대안, 질문

          질의 사항: 
               - 현재 sema_down은 busy wating이 아닌지? => block에서 schedule을 호출하므로 상관 없는지
               - lock_try_acquire 활용 했는지? (인터럽트 핸들러 안에서 호출되어야 한다고 하던데, 고려했는지) 
               - 

*/
#include <stdio.h>
#include "tests/threads/tests.h"
#include "threads/init.h"
#include "threads/synch.h"
#include "threads/thread.h"

static thread_func acquire_thread_func;

void
test_priority_donate_lower (void) 
{
  struct lock lock;

  /* This test does not work with the MLFQS. */
  ASSERT (!thread_mlfqs);

  /* Make sure our priority is the default. */  
  ASSERT (thread_get_priority () == PRI_DEFAULT);

  lock_init (&lock);
  lock_acquire (&lock); 
  thread_create ("acquire", PRI_DEFAULT + 10, acquire_thread_func, &lock);
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 10, thread_get_priority ());
  msg ("Lowering base priority...");
  thread_set_priority (PRI_DEFAULT - 10); 
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT + 10, thread_get_priority ()); 
  lock_release (&lock); 
  msg ("acquire must already have finished.");
  msg ("Main thread should have priority %d.  Actual priority: %d.",
       PRI_DEFAULT - 10, thread_get_priority ()); 
}

static void
acquire_thread_func (void *lock_) 
{
  struct lock *lock = lock_;

  lock_acquire (lock); 
  msg ("acquire: got the lock");
  lock_release (lock); 
  msg ("acquire: done");
}
