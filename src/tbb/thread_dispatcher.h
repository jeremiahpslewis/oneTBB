/*
    Copyright (c) 2020-2022 Intel Corporation

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#ifndef _TBB_thread_dispatcher_H
#define _TBB_thread_dispatcher_H

#include "oneapi/tbb/detail/_config.h"
#include "oneapi/tbb/detail/_utils.h"
#include "oneapi/tbb/rw_mutex.h"
#include "arena.h"
#include "governor.h"
#include "thread_data.h"
#include "rml_tbb.h"
#include "clients.h"

class threading_control;

namespace tbb {
namespace detail {
namespace r1 {

class thread_dispatcher : no_copy, rml::tbb_client {
    using client_list_type = intrusive_list<thread_dispatcher_client>;
    using client_list_mutex_type = d1::rw_mutex;
public:
    thread_dispatcher(threading_control& tc, unsigned hard_limit, std::size_t stack_size)
        : my_threading_control(tc)
        , my_num_workers_hard_limit(hard_limit)
        , my_stack_size(stack_size)
    {
        my_server = governor::create_rml_server( *this );
        __TBB_ASSERT( my_server, "Failed to create RML server" );
    }

    ~thread_dispatcher() {
        poison_pointer(my_server);
    }

    thread_dispatcher_client* select_next_client(thread_dispatcher_client* hint) {
        unsigned next_client_priority_level = num_priority_levels;
        if (hint) {
            next_client_priority_level = hint->priority_level();
        }

        for (unsigned idx = 0; idx < next_client_priority_level; ++idx) {
            if (!my_client_list[idx].empty()) {
                return &*my_client_list[idx].begin();
            }
        }

        return hint;
    }

    thread_dispatcher_client* create_client(arena& a) {
        return new (cache_aligned_allocate(sizeof(thread_dispatcher_client))) thread_dispatcher_client(a, my_clients_aba_epoch);
    }

    void register_client(thread_dispatcher_client* client) {
        client_list_mutex_type::scoped_lock lock(my_list_mutex);
        insert_client(*client);
    }

    bool try_unregister_client(thread_dispatcher_client* client, std::uint64_t aba_epoch, unsigned priority) {
        __TBB_ASSERT(client, nullptr);
        // we hold reference to the server, so market cannot be destroyed at any moment here
        __TBB_ASSERT(!is_poisoned(my_server), nullptr);
        my_list_mutex.lock();
        client_list_type::iterator it = my_client_list[priority].begin();
        for ( ; it != my_client_list[priority].end(); ++it ) {
            if (client == &*it) {
                if (it->get_aba_epoch() == aba_epoch) {
                    // Client is alive
                    // Acquire my_references to sync with threads that just left the arena
                    // Pay attention that references should be read before workers_requested because
                    // if references is no zero some other thread might call adjust_demand and lead to
                    // a race over workers_requested
                    if (!client->references() && !client->num_workers_requested()) {
                        /*__TBB_ASSERT(
                            !a->my_num_workers_allotted.load(std::memory_order_relaxed) &&
                            (a->my_pool_state == arena::SNAPSHOT_EMPTY || !a->my_max_num_workers),
                            "Inconsistent arena state"
                        );*/

                        // Arena is abandoned. Destroy it.
                        remove_client(*client);
                        ++my_clients_aba_epoch;

                        my_list_mutex.unlock();

                        client->~thread_dispatcher_client();
                        cache_aligned_deallocate(client);

                        return true;
                    }
                }
                break;
            }
        }
        my_list_mutex.unlock();
        return false;
    }

    void insert_client(thread_dispatcher_client& client) {
        __TBB_ASSERT(client.priority_level() < num_priority_levels, nullptr);
        my_client_list[client.priority_level()].push_front(client);

        __TBB_ASSERT(!my_next_client || my_next_client->priority_level() < num_priority_levels, nullptr);
        my_next_client = select_next_client(my_next_client);
    }

    void remove_client(thread_dispatcher_client& client) {
        __TBB_ASSERT(client.priority_level() < num_priority_levels, nullptr);
        my_client_list[client.priority_level()].remove(client);

        if (my_next_client == &client) {
            my_next_client = nullptr;
        }
        my_next_client = select_next_client(my_next_client);
    }

    bool is_client_in_list(client_list_type& clients, thread_dispatcher_client* client) {
        __TBB_ASSERT(client, "Expected non-null pointer to client.");
        for (client_list_type::iterator it = clients.begin(); it != clients.end(); ++it) {
            if (client == &*it) {
                return true;
            }
        }
        return false;
    }

    bool is_client_alive(thread_dispatcher_client* client) {
        if (!client) {
            return false;
        }

        // Still cannot access internals of the arena since the object itself might be destroyed.
        for (unsigned idx = 0; idx < num_priority_levels; ++idx) {
            if (is_client_in_list(my_client_list[idx], client)) {
                return true;
            }
        }
        return false;
    }

    thread_dispatcher_client* client_in_need(client_list_type* clients, thread_dispatcher_client* hint) {
        // TODO: make sure arena with higher priority returned only if there are available slots in it.
        hint = select_next_client(hint);
        if (!hint) {
            return nullptr;
        }

        client_list_type::iterator it = hint;
        unsigned curr_priority_level = hint->priority_level();
        __TBB_ASSERT(it != clients[curr_priority_level].end(), nullptr);
        do {
            thread_dispatcher_client& t = *it;
            if (++it == clients[curr_priority_level].end()) {
                do {
                    ++curr_priority_level %= num_priority_levels;
                } while (clients[curr_priority_level].empty());
                it = clients[curr_priority_level].begin();
            }
            if (t.try_join()) {
                return &t;
            }
        } while (it != hint);
        return nullptr;
    }


    thread_dispatcher_client* client_in_need(thread_dispatcher_client* prev) {
        client_list_mutex_type::scoped_lock lock(my_list_mutex, /*is_writer=*/false);
        if (is_client_alive(prev)) {
            return client_in_need(my_client_list, prev);
        }
        return client_in_need(my_client_list, my_next_client);
    }

    void process(job& j) override {
        thread_data& td = static_cast<thread_data&>(j);
        // td.my_last_client can be dead. Don't access it until arena_in_need is called
        thread_dispatcher_client* client = td.my_last_client;
        for (int i = 0; i < 2; ++i) {
            while ((client = client_in_need(client)) ) {
                td.my_last_client = client;
                client->process(td);
            }
            // Workers leave thread_dispatcher because there is no arena in need. It can happen earlier than
            // adjust_job_count_estimate() decreases my_slack and RML can put this thread to sleep.
            // It might result in a busy-loop checking for my_slack<0 and calling this method instantly.
            // the yield refines this spinning.
            if ( !i ) {
                yield();
            }
        }
    }

    void adjust_job_count_estimate(int delta) {
        if (delta != 0) {
            my_server->adjust_job_count_estimate(delta);
        }
    }

    void cleanup(job& j) override;

    void acknowledge_close_connection() override;

    ::rml::job* create_one_job() override;


    //! Used when RML asks for join mode during workers termination.
    bool must_join_workers () const { return my_join_workers; }

    //! Returns the requested stack size of worker threads.
    std::size_t worker_stack_size() const { return my_stack_size; }

    version_type version () const override { return 0; }

    unsigned max_job_count () const override { return my_num_workers_hard_limit; }

    std::size_t min_stack_size () const override { return worker_stack_size(); }

private:
    friend class threading_control;
    static constexpr unsigned num_priority_levels = 3;
    client_list_mutex_type my_list_mutex;
    client_list_type my_client_list[num_priority_levels];

    thread_dispatcher_client* my_next_client{nullptr};

    //! Shutdown mode
    bool my_join_workers{false};

    threading_control& my_threading_control;

    //! ABA prevention marker to assign to newly created arenas
    std::atomic<std::uint64_t> my_clients_aba_epoch{0};

    //! Maximal number of workers allowed for use by the underlying resource manager
    /** It can't be changed after market creation. **/
    unsigned my_num_workers_hard_limit{0};

    //! Stack size of worker threads
    std::size_t my_stack_size{0};

    //! First unused index of worker
    /** Used to assign indices to the new workers coming from RML **/
    std::atomic<unsigned> my_first_unused_worker_idx{0};

    //! Pointer to the RML server object that services this TBB instance.
    rml::tbb_server* my_server{nullptr};
};

} // namespace r1
} // namespace detail
} // namespace tbb

#endif // _TBB_thread_dispatcher_H