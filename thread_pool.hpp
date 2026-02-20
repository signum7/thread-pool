// ═══════════════════════════════════════════════════════════════════
//  thread_pool  +  smart dispatcher  |  C++20
//  Version: v1.2
//
//  Возможности:
//  [D1] ПРИОРИТЕТЫ      — std::priority_queue; Critical > High > Normal > Low
//  [D2] ЗАВИСИМОСТИ     — задача pending пока не завершены все deps
//  [D3] RETRY-ПОЛИТИКА  — авто-перезапуск при исключении (max_retries раз)
//  [D4] ОТМЕНА          — cancel(task_id) → bool
//  [D5] ГРУППЫ (RAII)   — make_group() → GroupHandle; ~GroupHandle = wait
//  [D6] СТАТИСТИКА      — stats() → PoolStats
//
//  Два API добавления задачи:
//    add_task(...)  — возвращает task_id (uint64_t); результат через wait_result.
//    submit(...)    — возвращает std::future<T>; task_id не нужен.
//
//  Thread-safety: все публичные операции защищены единым std::mutex.
//
//  Семантика отмены + зависимости:
//    cancel(A) НЕ отменяет каскадно задачи, зависящие от A.
//    Зависимые задачи будут выполнены после того, как A помечена cancelled
//    (resolve_locked вызывается и для cancelled).
//    Для каскадной отмены — явно отмените зависимые задачи вручную.
//
//  Семантика shutdown (~thread_pool):
//    Все незапущенные задачи (pending_ и rq_) отменяются.
//    Уже running задачи дожидаются завершения.
//

// ═══════════════════════════════════════════════════════════════════
#pragma once

#include <any>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>


// ────────────────────────────────────────────────────────────────────
// Публичные перечисления
// ────────────────────────────────────────────────────────────────────

/// Приоритет задачи. Critical — наивысший, Low — наименьший.
enum class Priority : int { Low = 0, Normal = 1, High = 2, Critical = 3 };

/// Состояние жизненного цикла задачи.
enum class TaskStatus {
    pending,    ///< Ждёт завершения зависимостей
    in_queue,   ///< В очереди готовых к запуску
    running,    ///< Выполняется воркером прямо сейчас
    completed,  ///< Завершена успешно
    failed,     ///< Завершена с исключением
    cancelled   ///< Отменена (cancel() или shutdown)
};

/// Состояние пула потоков.
enum class PoolState { running, draining, stopped };


// ────────────────────────────────────────────────────────────────────
// TaskOptions — опции добавляемой задачи
//
// Поля строго в этом порядке: GCC/Clang требуют его для designated init.
// ────────────────────────────────────────────────────────────────────

/// Параметры задачи, передаваемые в add_task() / submit().
struct TaskOptions {
    Priority              priority    = Priority::Normal; ///< Приоритет в очереди
    uint32_t              max_retries = 0;                ///< Макс. число повторных попыток при исключении
    std::vector<uint64_t> depends_on  = {};               ///< task_id, которые должны завершиться до запуска
    std::string           name        = {};               ///< Человекочитаемое имя (для отладки/stats)
    uint64_t              group_id    = 0;                ///< Группа (0 = нет группы); см. make_group()

    // Примечание: auto_cleanup — внутренняя деталь реализации submit().
    // Намеренно убрано из публичного API. Используйте submit() для задач
    // с автоматической очисткой записи.
};


// ────────────────────────────────────────────────────────────────────
// PoolStats — снапшот метрик пула
// ────────────────────────────────────────────────────────────────────

/// Метрики пула на момент вызова stats().
/// Примечание: active_workers может расходиться на ±1 (воркер уменьшает
/// active_ до повторного захвата мьютекса при переходе к следующей задаче).
struct PoolStats {
    uint64_t ready_queue_size   = 0; ///< Задач в priority_queue (готовы к запуску)
    uint64_t pending_deps_count = 0; ///< Задач, ожидающих зависимостей
    uint32_t active_workers     = 0; ///< Воркеров, выполняющих задачи прямо сейчас
    uint64_t total_submitted    = 0; ///< Всего добавлено задач (= следующий свободный id)
    uint64_t total_completed    = 0; ///< Завершено успешно
    uint64_t total_failed       = 0; ///< Завершено с ошибкой (после всех retry)
    uint64_t total_cancelled    = 0; ///< Отменено

    void print(std::ostream& os = std::cout) const {
        os << "─── PoolStats ───────────────────\n"
           << "  ready_queue : " << ready_queue_size   << '\n'
           << "  pending_deps: " << pending_deps_count << '\n'
           << "  active      : " << active_workers     << '\n'
           << "  submitted   : " << total_submitted    << '\n'
           << "  completed   : " << total_completed    << '\n'
           << "  failed      : " << total_failed       << '\n'
           << "  cancelled   : " << total_cancelled    << '\n'
           << "─────────────────────────────────\n";
    }
};


// ────────────────────────────────────────────────────────────────────
// Task — тип-стёртая обёртка над вызываемым объектом
//
// Хранит fn_ как std::function<std::any()>: decay-копии F и всех Args
// захватываются в замыкании при конструировании; std::apply разворачивает
// их при вызове. Void-функции возвращают пустой std::any{}.
// ────────────────────────────────────────────────────────────────────
class Task {
public:
    Task() = default;

    /// Конструирует задачу из вызываемого объекта f и аргументов args.
    /// Все аргументы копируются/перемещаются по значению (decay_copy).
    template <typename F, typename... Args>
    Task(F&& f, Args&&... args) {
        using Fn  = std::decay_t<F>;
        using Tup = std::tuple<std::decay_t<Args>...>;
        using R   = std::invoke_result_t<Fn, std::decay_t<Args>...>;

        is_void_ = std::is_void_v<R>;

        fn_ = [fn  = Fn(std::forward<F>(f)),
               tup = Tup(std::forward<Args>(args)...)]() mutable -> std::any {
            if constexpr (std::is_void_v<R>) {
                std::apply(fn, tup);
                return {};                      // void → пустой any
            } else {
                return std::any{ std::apply(fn, tup) };
            }
        };
    }

    /// Выполняет задачу. Возвращает std::any{result} или std::any{} для void.
    /// Исключения не перехватываются — пробрасываются наружу.
    std::any operator()() { return fn_ ? fn_() : std::any{}; }

    /// true, если задача возвращает не-void тип.
    bool has_result() const { return !is_void_; }

private:
    std::function<std::any()> fn_;
    bool                      is_void_ = true;
};


// ────────────────────────────────────────────────────────────────────
// Внутренние структуры данных
// ────────────────────────────────────────────────────────────────────

/// Метаданные задачи, хранимые в infos_ на протяжении её жизненного цикла.
struct TaskInfo {
    TaskStatus         status       = TaskStatus::pending;
    bool               consumed     = false; ///< true после первого wait/wait_result; run() не erase consumed
    std::any           result;               ///< Результат (для non-void add_task)
    std::exception_ptr error;               ///< Сохранённое исключение (status==failed)
    std::string        name;
    uint32_t           attempts     = 0;
    uint64_t           group_id     = 0;
    bool               auto_cleanup = false; ///< Внутренний флаг: удалить запись после завершения (submit)
};

/// Запись в priority_queue готовых к запуску задач.
/// Task хранится через shared_ptr — нет дорогой копии std::function при push/pop.
struct ReadyEntry {
    Priority              priority     = Priority::Normal;
    uint64_t              task_id      = 0;
    std::shared_ptr<Task> task;
    uint32_t              retries_left = 0;

    /// Меньший приоритет → меньше в heap (std::priority_queue — max-heap).
    /// При равном приоритете: меньший task_id → "старше" → выше приоритет.
    bool operator<(const ReadyEntry& o) const {
        if (priority != o.priority)
            return static_cast<int>(priority) < static_cast<int>(o.priority);
        return task_id > o.task_id;
    }
};

/// Запись задачи, ожидающей завершения зависимостей.
struct PendingEntry {
    std::shared_ptr<Task>        task;
    uint64_t                     task_id      = 0;
    Priority                     priority     = Priority::Normal;
    uint32_t                     retries_left = 0;
    std::unordered_set<uint64_t> remaining_deps; ///< Незавершённые зависимости
};

/// Состояние группы задач [D5].
struct GroupInfo {
    std::unordered_set<uint64_t> pending_ids; ///< Id задач группы ещё не в terminal-статусе
    bool sealed = false; ///< true после вызова GroupHandle::wait()
};


// ────────────────────────────────────────────────────────────────────
// Forward declaration
// ────────────────────────────────────────────────────────────────────
class thread_pool;


// ────────────────────────────────────────────────────────────────────
// GroupHandle — RAII-дескриптор группы задач [D5]
//
// Создаётся через thread_pool::make_group().
// При выходе из scope деструктор неявно вызывает wait() (seal + ожидание).
// Не копируется. Перемещается (moved-from объект помечает done_=true).
// ────────────────────────────────────────────────────────────────────
class GroupHandle {
public:
    GroupHandle(std::shared_ptr<thread_pool> pool, uint64_t gid)
        : pool_(std::move(pool)), gid_(gid) {}

    /// Деструктор: если wait() ещё не вызван — ждёт все задачи группы.
    ~GroupHandle() {
        if (!done_) {
            try { wait(); } catch (...) {}
        }
    }

    /// Move-конструктор: moved-from объект помечается как done_, чтобы
    /// его деструктор не вызвал wait() через нулевой pool_.
    GroupHandle(GroupHandle&& o) noexcept
        : pool_(std::move(o.pool_)), gid_(o.gid_), done_(o.done_)
    {
        o.done_ = true; // moved-from больше не владеет группой
    }

    GroupHandle(const GroupHandle&)            = delete;
    GroupHandle& operator=(const GroupHandle&) = delete;
    GroupHandle& operator=(GroupHandle&&)      = delete;

    /// Добавляет задачу в группу (с опциями по умолчанию).
    template <typename Func, typename... Args>
    uint64_t add_task(Func&& f, Args&&... a);

    /// Добавляет задачу в группу с явными опциями.
    /// opts.group_id будет перезаписан id этой группы.
    template <typename Func, typename... Args>
    uint64_t add_task(TaskOptions opts, Func&& f, Args&&... a);

    /// Запечатывает группу и блокирует поток до завершения всех задач группы.
    /// После вызова handle считается исчерпанным (done_=true).
    void     wait();

    uint64_t id() const { return gid_; }

private:
    std::shared_ptr<thread_pool> pool_;
    uint64_t                     gid_;
    bool                         done_ = false;
};


// ────────────────────────────────────────────────────────────────────
// thread_pool — основной класс
//
// Создаётся только через thread_pool::create(n) — возвращает shared_ptr.
// Это обязательно для корректной работы enable_shared_from_this (GroupHandle).
//
// Thread-safety: все публичные методы защищены mtx_.
// ────────────────────────────────────────────────────────────────────
class thread_pool : public std::enable_shared_from_this<thread_pool> {
    friend class GroupHandle;

    /// Закрытый токен — запрет прямого вызова конструктора вне create().
    struct ctor_token {};

public:
    /// Единственный способ создания пула. Запускает n воркеров.
    static std::shared_ptr<thread_pool> create(uint32_t n) {
        return std::make_shared<thread_pool>(ctor_token{}, n);
    }

    /// Публичный конструктор (нужен make_shared), но доступен только через create().
    thread_pool(ctor_token, uint32_t n) {
        threads_.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
            threads_.emplace_back(&thread_pool::run, this);
    }

    thread_pool(const thread_pool&)            = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool(thread_pool&&)                 = delete;
    thread_pool& operator=(thread_pool&&)      = delete;


    // ── add_task ────────────────────────────────────────────────────
    //
    // Добавляет задачу в пул и возвращает task_id (uint64_t).
    // Результат доступен через wait_result(id).
    // Зависит от opts.depends_on — задача будет pending пока они не завершены.
    //
    // Исключения:
    //   std::runtime_error  — пул завершается (draining/stopped)
    //   std::invalid_argument — неизвестный group_id или dep id
    //   std::runtime_error  — группа уже запечатана

    /// Добавляет задачу с параметрами по умолчанию.
    template <typename F, typename... Args>
    uint64_t add_task(F&& f, Args&&... a) {
        return add_task(TaskOptions{}, std::forward<F>(f), std::forward<Args>(a)...);
    }

    /// Добавляет задачу с явными опциями.
    template <typename F, typename... Args>
    uint64_t add_task(TaskOptions opts, F&& f, Args&&... a) {
        // Построение Task допустимо под мьютексом: конструктор только копирует
        // аргументы, не вызывает пользовательский код.
        auto task_ptr = std::make_shared<Task>(std::forward<F>(f), std::forward<Args>(a)...);

        std::unique_lock<std::mutex> lk(mtx_);

        if (state_.load() != PoolState::running)
            throw std::runtime_error("thread_pool: пул завершается");

        return enqueue_locked(std::move(opts), /*auto_cleanup=*/false, std::move(task_ptr));
    }


    // ── submit ──────────────────────────────────────────────────────
    //
    // Типобезопасный API: возвращает std::future<T>.
    // task_id не возвращается — записи infos_ удаляются автоматически.
    // Исключения задачи попадают в future (fut.get() ре-бросит их).
    //
    // [[nodiscard]]: игнорирование future приведёт к потере исключений задачи.

    /// Добавляет задачу без опций. Возвращает std::future<T>.
    template <typename F, typename... Args>
    [[nodiscard]]
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
    {
        // Делегируем в submit(opts, ...) — единственный code path.
        return submit(TaskOptions{}, std::forward<F>(f), std::forward<Args>(args)...);
    }

    /// Добавляет задачу с явными опциями. Возвращает std::future<T>.
    template <typename F, typename... Args>
    [[nodiscard]]
    auto submit(TaskOptions opts, F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<std::decay_t<F>, std::decay_t<Args>...>>
    {
        using Fn = std::decay_t<F>;
        using R  = std::invoke_result_t<Fn, std::decay_t<Args>...>;

        // Связываем функцию и аргументы в bound-вызов.
        auto bound = [func = Fn(std::forward<F>(f)),
                      tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable -> R {
            if constexpr (std::is_void_v<R>)
                std::apply(func, tup);
            else
                return std::apply(func, tup);
        };

        // packaged_task транспортирует результат/исключение в future.
        // Хранится через shared_ptr — lifetime гарантирован до вызова.
        auto packaged = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        std::future<R> fut = packaged->get_future();

        // Task-обёртка вызывает packaged_task; исключения уйдут в future,
        // а не в воркер — run() не увидит failed-состояния.
        auto wrapper = [packaged]() { (*packaged)(); };
        auto task_ptr = std::make_shared<Task>(std::move(wrapper));

        {
            std::unique_lock<std::mutex> lk(mtx_);

            if (state_.load() != PoolState::running)
                throw std::runtime_error("thread_pool: пул завершается");

            // auto_cleanup=true: infos_ запись будет удалена после выполнения,
            // т.к. task_id не возвращается и wait_result никогда не вызовется.
            enqueue_locked(std::move(opts), /*auto_cleanup=*/true, std::move(task_ptr));
        }

        return fut;
    }


    // ── cancel [D4] ─────────────────────────────────────────────────
    //
    // Отменяет задачу до её запуска.
    // Возвращает true  — задача отменена успешно.
    // Возвращает false — задача уже running или в terminal-статусе.
    //
    // Каскадной отмены зависимых задач НЕТ: зависимые задачи будут
    // выполнены после того, как отменённая "разрешится" через resolve_locked.
    //
    // Исключения:
    //   std::invalid_argument — неизвестный task_id

    bool cancel(uint64_t id) {
        std::unique_lock<std::mutex> lk(mtx_);

        auto it = infos_.find(id);
        if (it == infos_.end())
            throw std::invalid_argument(
                "cancel: неизвестный task_id=" + std::to_string(id));

        if (it->second.status == TaskStatus::running || terminal(it->second.status))
            return false; // уже поздно или уже terminal

        const uint64_t gid = it->second.group_id;
        it->second.status  = TaskStatus::cancelled;
        ++stat_cancelled_;

        // Если задача была pending — почистить dep_waiters_ от её ссылок.
        // Это предотвращает разрастание dep_waiters_ при массовых отменах.
        if (auto pit = pending_.find(id); pit != pending_.end()) {
            for (uint64_t dep : pit->second.remaining_deps) {
                auto& vec = dep_waiters_[dep];
                vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
                if (vec.empty())
                    dep_waiters_.erase(dep);
            }
            pending_.erase(pit);
        }

        // Разблокировать зависимые задачи (они не отменяются каскадно).
        resolve_locked(id);
        group_finish_locked(gid, id);
        cv_.notify_all();
        return true;
    }


    // ── wait ────────────────────────────────────────────────────────
    //
    // Блокирует поток до завершения задачи (любой terminal-статус).
    // После возврата запись в infos_ удалена; повторный вызов бросает исключение.
    //
    // Исключения:
    //   std::invalid_argument — неизвестный id (или уже consumed)
    //   std::runtime_error    — double-wait (consumed=true)

    void wait(uint64_t id) {
        std::unique_lock<std::mutex> lk(mtx_);
        check_id_locked(id);

        auto& info = infos_.at(id);
        if (info.consumed)
            throw std::runtime_error(
                "thread_pool: task id=" + std::to_string(id) + " already consumed");
        info.consumed = true;

        // Используем find вместо at: запись может исчезнуть только если
        // auto_cleanup=true И !consumed — но мы только что поставили consumed=true,
        // поэтому run() не сотрёт запись. Предикат через find — defensive check.
        cv_.wait(lk, [&] {
            auto it = infos_.find(id);
            return it == infos_.end() || terminal(it->second.status);
        });

        infos_.erase(id);
    }


    // ── wait_result → std::any ───────────────────────────────────────
    //
    // Блокирует поток до завершения задачи и возвращает результат.
    // После возврата запись удалена; повторный вызов бросает исключение.
    //
    // Бросает:
    //   Оригинальное исключение задачи, если status == failed.
    //   std::runtime_error("task cancelled"), если status == cancelled.
    //   std::invalid_argument — неизвестный id.
    //   std::runtime_error    — double-wait (consumed=true).

    std::any wait_result(uint64_t id) {
        std::unique_lock<std::mutex> lk(mtx_);
        check_id_locked(id);

        auto& info = infos_.at(id);
        if (info.consumed)
            throw std::runtime_error(
                "thread_pool: task id=" + std::to_string(id) + " already consumed");
        info.consumed = true;

        // consumed=true → run() не сотрёт запись (do_erase проверяет !consumed).
        // Используем find для defensive-проверки на случай будущих изменений.
        cv_.wait(lk, [&] {
            auto it = infos_.find(id);
            return it == infos_.end() || terminal(it->second.status);
        });

        auto it = infos_.find(id);
        if (it == infos_.end()) {
            // Не должно случаться при consumed=true, но defensive throw лучше
            // чем молчаливый возврат пустого any.
            throw std::runtime_error(
                "thread_pool: task id=" + std::to_string(id)
                + " record disappeared unexpectedly");
        }

        auto& i = it->second;

        if (i.status == TaskStatus::failed) {
            std::exception_ptr ep = i.error;
            infos_.erase(id);
            std::rethrow_exception(ep); // бросает оригинальное исключение задачи
        }

        if (i.status == TaskStatus::cancelled) {
            infos_.erase(id);
            throw std::runtime_error(
                "thread_pool: task id=" + std::to_string(id) + " was cancelled");
        }

        std::any res = std::move(i.result);
        infos_.erase(id);
        return res;
    }

    /// Типизированная версия wait_result. Удобна как: pool->wait_result(id, value).
    template <class T>
    void wait_result(uint64_t id, T& v) {
        v = std::any_cast<T>(wait_result(id));
    }


    // ── wait_all ────────────────────────────────────────────────────
    //
    // Ждёт, пока суммарное число завершённых (completed+failed+cancelled)
    // достигнет снапшота last_id_ на момент вызова.
    //
    // cleanup=true: очищает infos_ после ожидания.
    //   ВАЖНО: после cleanup=true все ранее полученные task_id становятся
    //   невалидными. Повторный wait/wait_result по ним бросит invalid_argument.

    void wait_all(bool cleanup = false) {
        std::unique_lock<std::mutex> lk(mtx_);

        // Снапшот берётся под мьютексом — нет гонки с параллельными add_task.
        const uint64_t expected = last_id_.load();

        cv_.wait(lk, [&] {
            return stat_completed_ + stat_failed_ + stat_cancelled_ >= expected;
        });

        if (cleanup) infos_.clear();
    }


    /// Неблокирующая проверка: завершилась ли задача успешно.
    /// Не потребляет запись (repeated calls allowed).
    /// Вернёт false для consumed-задач (запись уже удалена).
    bool calculated(uint64_t id) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = infos_.find(id);
        return it != infos_.end() && it->second.status == TaskStatus::completed;
    }


    // ── make_group [D5] ─────────────────────────────────────────────
    //
    // Создаёт новую группу и возвращает RAII-дескриптор.
    // Деструктор GroupHandle вызывает wait() (seal + ожидание всех задач группы).
    //
    // Использование:
    //   auto g = pool->make_group();
    //   g.add_task(fn1);
    //   g.add_task(TaskOptions{.priority=Priority::High}, fn2);
    //   // при выходе из scope: g.wait() → ждёт fn1 и fn2

    GroupHandle make_group() {
        std::lock_guard<std::mutex> lk(mtx_);
        const uint64_t gid = last_gid_++;
        groups_[gid] = GroupInfo{};
        return GroupHandle(shared_from_this(), gid);
    }


    // ── stats [D6] ──────────────────────────────────────────────────

    /// Возвращает снапшот метрик пула (под мьютексом).
    PoolStats stats() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return {
            (uint64_t)rq_.size(),
            (uint64_t)pending_.size(),
            active_.load(),
            last_id_.load(),
            stat_completed_,
            stat_failed_,
            stat_cancelled_
        };
    }


    // ── Деструктор ──────────────────────────────────────────────────
    //
    // Переводит пул в draining: все незапущенные задачи (pending_ и rq_)
    // отменяются. Запущенные задачи дожидаются завершения.
    // Воркеры получают notify_all и завершают работу.

    ~thread_pool() {
        {
            std::unique_lock<std::mutex> lk(mtx_);
            state_.store(PoolState::draining);

            // Отмена pending-задач (ожидают зависимостей — в rq_ не попали).
            for (auto& [task_id, _] : pending_) {
                auto it = infos_.find(task_id);
                if (it != infos_.end() && !terminal(it->second.status)) {
                    it->second.status = TaskStatus::cancelled;
                    ++stat_cancelled_;
                }
            }
            pending_.clear();

            // Отмена задач, уже лежащих в очереди готовых (rq_).
            while (!rq_.empty()) {
                auto e = rq_.top(); rq_.pop();
                auto it = infos_.find(e.task_id);
                if (it != infos_.end() && !terminal(it->second.status)) {
                    it->second.status = TaskStatus::cancelled;
                    ++stat_cancelled_;
                }
            }
        }
        cv_.notify_all();

        for (auto& t : threads_) t.join();
        state_.store(PoolState::stopped);
    }


private:
    // ────────────────────────────────────────────────────────────────
    // Вспомогательные методы (REQUIRES: mtx_ held, если не указано иное)
    // ────────────────────────────────────────────────────────────────

    /// Возвращает true, если статус является конечным (нет смысла ждать дальше).
    static bool terminal(TaskStatus s) noexcept {
        return s == TaskStatus::completed
            || s == TaskStatus::failed
            || s == TaskStatus::cancelled;
    }

    /// Бросает invalid_argument, если id отсутствует в infos_.
    void check_id_locked(uint64_t id) {
        if (!infos_.count(id))
            throw std::invalid_argument(
                "thread_pool: неизвестный/consumed id=" + std::to_string(id));
    }

    /// Помещает ReadyEntry в priority_queue и уведомляет один поток-воркер.
    void push_ready_lk(ReadyEntry&& e) {
        rq_.push(std::move(e));
        cv_.notify_one();
    }


    // ── enqueue_locked ───────────────────────────────────────────────
    //
    // REQUIRES: mtx_ held, state_ == running.
    //
    // Единственная точка регистрации задачи в infos_ и очередях.
    // Принимает уже построенный task_ptr; auto_cleanup — внутренний флаг
    // (true для submit, false для add_task).
    //
    // Exception safety: валидация выполняется ДО любой мутации state,
    // поэтому при исключении состояние пула остаётся консистентным.

    uint64_t enqueue_locked(TaskOptions opts, bool auto_cleanup, std::shared_ptr<Task> task_ptr) {
        // ── Фаза валидации (не мутирует state) ───────────────────────

        if (opts.group_id != 0) {
            auto git = groups_.find(opts.group_id);
            if (git == groups_.end())
                throw std::invalid_argument(
                    "add_task: несуществующий group_id=" + std::to_string(opts.group_id));
            if (git->second.sealed)
                throw std::runtime_error(
                    "add_task: группа group_id=" + std::to_string(opts.group_id)
                    + " уже запечатана");
        }

        for (uint64_t dep : opts.depends_on) {
            if (!infos_.count(dep))
                throw std::invalid_argument(
                    "add_task: зависимость id=" + std::to_string(dep)
                    + " неизвестна (или уже consumed)");
        }

        // ── Фаза коммита (исключения маловероятны — только std::bad_alloc) ──

        const uint64_t id = last_id_++;

        TaskInfo& info    = infos_[id];
        info.name         = opts.name.empty() ? "#" + std::to_string(id) : std::move(opts.name);
        info.group_id     = opts.group_id;
        info.auto_cleanup = auto_cleanup;

        if (opts.group_id != 0)
            groups_[opts.group_id].pending_ids.insert(id);

        // Фильтруем зависимости: уже terminal-задачи не блокируют запуск.
        std::unordered_set<uint64_t> live_deps;
        for (uint64_t dep : opts.depends_on) {
            if (!terminal(infos_[dep].status))
                live_deps.insert(dep);
        }

        if (live_deps.empty()) {
            // Все зависимости уже выполнены (или их не было) → сразу в очередь.
            info.status = TaskStatus::in_queue;
            push_ready_lk({ opts.priority, id, std::move(task_ptr), opts.max_retries });
        } else {
            // Задача ждёт. Строим обратный индекс dep → [ждущие задачи].
            info.status = TaskStatus::pending;
            for (uint64_t dep : live_deps)
                dep_waiters_[dep].push_back(id);
            pending_[id] = { std::move(task_ptr), id, opts.priority, opts.max_retries, std::move(live_deps) };
        }

        return id;
    }


    // ── resolve_locked ───────────────────────────────────────────────
    //
    // Вызывается после перехода finished_id в terminal-статус (включая cancelled).
    // Проходит по обратному индексу dep_waiters_[finished_id] и промоутирует
    // задачи, у которых больше нет незавершённых зависимостей.
    // Сложность: O(waiters), не O(all pending).

    void resolve_locked(uint64_t finished_id) {
        auto wit = dep_waiters_.find(finished_id);
        if (wit == dep_waiters_.end()) return;

        std::vector<uint64_t> promote;
        for (uint64_t pid : wit->second) {
            auto pit = pending_.find(pid);
            if (pit == pending_.end()) continue; // уже отменена или промоутирована

            pit->second.remaining_deps.erase(finished_id);
            if (pit->second.remaining_deps.empty())
                promote.push_back(pid);
        }
        dep_waiters_.erase(wit); // индекс для finished_id больше не нужен

        for (uint64_t pid : promote) {
            auto  node = pending_.extract(pid);
            auto& pe   = node.mapped();
            auto& ti   = infos_[pid];
            if (ti.status == TaskStatus::cancelled) continue; // могла быть отменена пока ждала
            ti.status = TaskStatus::in_queue;
            push_ready_lk({ pe.priority, pid, std::move(pe.task), pe.retries_left });
        }
    }

    /// Удаляет task_id из pending_ids группы gid. No-op если gid==0.
    void group_finish_locked(uint64_t gid, uint64_t task_id) {
        if (gid == 0) return;
        auto git = groups_.find(gid);
        if (git == groups_.end()) return;
        git->second.pending_ids.erase(task_id);
        // cv_.notify_all() вызовет родитель (run()), не мы
    }

    /// Запечатывает группу и блокирует поток до завершения всех её задач.
    /// После возврата запись группы удаляется из groups_.
    void wait_group_impl(uint64_t gid) {
        std::unique_lock<std::mutex> lk(mtx_);

        // Если группа уже неизвестна — считаем, что она завершена.
        auto git = groups_.find(gid);
        if (git == groups_.end()) return;

        // at() вместо operator[]: не создаём запись молчаливо если её нет.
        groups_.at(gid).sealed = true;

        cv_.wait(lk, [&] {
            auto it = groups_.find(gid);
            return it == groups_.end()
                || (it->second.sealed && it->second.pending_ids.empty());
        });

        groups_.erase(gid);
    }


    // ── run — основной цикл воркера ──────────────────────────────────
    //
    // Воркер крутится в while(true):
    //   1. Ждёт задачу в rq_ или сигнал draining.
    //   2. Извлекает ReadyEntry, проверяет статус задачи.
    //   3. Снимает лок, выполняет задачу.
    //   4. Захватывает лок, обновляет TaskInfo.
    //   5. При failed + retries_left > 0 — возвращает задачу в rq_.
    //   6. Иначе — переводит в terminal, зовёт resolve/group, notify_all.
    //
    // КРИТИЧНО: задача выполняется строго ВНЕ мьютекса.

    void run() {
        while (true) {
            std::unique_lock<std::mutex> lk(mtx_);

            // Ждём задачу или сигнал о shutdown.
            cv_.wait(lk, [this] {
                return !rq_.empty() || state_.load() != PoolState::running;
            });

            // Очередь пуста и пул завершается — воркер выходит.
            if (rq_.empty()) break;

            ReadyEntry e = rq_.top();
            rq_.pop();
            const uint64_t tid = e.task_id;

            // Запись могла исчезнуть (например: wait_all(cleanup=true) во время retry)
            // или задача была отменена после помещения в rq_ (lazy-cancel).
            auto it = infos_.find(tid);
            if (it == infos_.end() || it->second.status == TaskStatus::cancelled) {
                cv_.notify_all(); // разбудить ожидающих этой задачи
                continue;
            }

            it->second.status = TaskStatus::running;
            it->second.attempts++;
            ++active_;
            lk.unlock(); // ← мьютекс отпущен: задача выполняется параллельно

            // ── Выполнение задачи ─────────────────────────────────────
            std::any   task_result;
            bool       failed = false;
            std::exception_ptr eptr;
            try {
                task_result = (*e.task)();
            } catch (...) {
                failed = true;
                eptr   = std::current_exception();
            }
            // ─────────────────────────────────────────────────────────

            --active_;
            lk.lock();

            // Запись могла исчезнуть пока мы работали (wait_all(cleanup=true)).
            auto it2 = infos_.find(tid);
            if (it2 == infos_.end()) {
                cv_.notify_all();
                continue;
            }
            auto& info = it2->second;

            if (failed && e.retries_left > 0) {
                // Есть попытки — возвращаем задачу в очередь.
                info.status = TaskStatus::in_queue;
                --e.retries_left;
                push_ready_lk(std::move(e));
                // Без notify: push_ready_lk уже уведомит один воркер.
            } else {
                // Итоговое состояние задачи.
                if (failed) {
                    info.status = TaskStatus::failed;
                    info.error  = eptr;
                    ++stat_failed_;
                } else {
                    if (e.task->has_result()) info.result = std::move(task_result);
                    info.status = TaskStatus::completed;
                    ++stat_completed_;
                }

                // Разблокировать зависимые задачи.
                resolve_locked(tid);

                // Уведомить группу.
                group_finish_locked(info.group_id, tid);

                // auto_cleanup: удалить запись, если никто её не ждёт.
                // Проверяем !consumed: если кто-то вызвал wait/wait_result,
                // они пометили consumed=true — нельзя erase (они ещё читают запись).
                const bool do_erase = info.auto_cleanup && !info.consumed;
                if (do_erase) infos_.erase(tid);
                // Не обращаемся к info после erase — dangling reference.

                cv_.notify_all();
            }
        }
    }


    // ────────────────────────────────────────────────────────────────
    // Поля
    // ────────────────────────────────────────────────────────────────

    std::vector<std::thread>                             threads_;
    mutable std::mutex                                   mtx_;
    std::condition_variable                              cv_;

    std::priority_queue<ReadyEntry>                      rq_;          ///< [D1] Очередь готовых задач
    std::unordered_map<uint64_t, PendingEntry>           pending_;     ///< [D2] Задачи с незавершёнными deps
    std::unordered_map<uint64_t, std::vector<uint64_t>>  dep_waiters_; ///< Обратный индекс: dep_id → [ждущие]
    std::unordered_map<uint64_t, TaskInfo>               infos_;       ///< Метаданные всех живых задач
    std::unordered_map<uint64_t, GroupInfo>              groups_;      ///< [D5] Активные группы
    uint64_t                                             last_gid_ = 1;///< Генератор group_id (0 зарезервирован)

    std::atomic<PoolState>  state_  { PoolState::running };
    std::atomic<uint64_t>   last_id_{ 0 }; ///< Генератор task_id; также = total submitted
    std::atomic<uint32_t>   active_ { 0 }; ///< Воркеров, выполняющих задачу прямо сейчас

    // Счётчики статистики (защищены mtx_).
    uint64_t stat_completed_ = 0;
    uint64_t stat_failed_    = 0;
    uint64_t stat_cancelled_ = 0;
};


// ────────────────────────────────────────────────────────────────────
// GroupHandle — реализация методов (после определения thread_pool)
// ────────────────────────────────────────────────────────────────────

template <typename Func, typename... Args>
uint64_t GroupHandle::add_task(Func&& f, Args&&... a) {
    return add_task(TaskOptions{}, std::forward<Func>(f), std::forward<Args>(a)...);
}

template <typename Func, typename... Args>
uint64_t GroupHandle::add_task(TaskOptions opts, Func&& f, Args&&... a) {
    opts.group_id = gid_; // принудительно привязываем к этой группе
    return pool_->add_task(std::move(opts), std::forward<Func>(f), std::forward<Args>(a)...);
}

void GroupHandle::wait() {
    done_ = true;
    pool_->wait_group_impl(gid_);
}