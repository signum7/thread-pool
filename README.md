# thread_pool

Лёгкий заголовочный (header-only) пул потоков для C++20 с приоритетами, зависимостями между задачами, retry-политикой, отменой, группами и статистикой.

```cpp
auto pool = thread_pool::create(4);

// fire-and-forget с результатом
auto fut = pool->submit([](int x){ return x * x; }, 7);
int result = fut.get(); // 49

// с приоритетом и зависимостями
uint64_t a = pool->add_task(TaskOptions{.priority = Priority::High}, compute);
uint64_t b = pool->add_task(TaskOptions{.depends_on = {a}},          save);
pool->wait_result(b, value);
```

---

## Содержание

- [Требования](#требования)
- [Установка](#установка)
- [Быстрый старт](#быстрый-старт)
- [Концепции](#концепции)
- [API](#api)
  - [Создание пула](#создание-пула)
  - [add_task — добавить задачу](#add_task--добавить-задачу)
  - [submit — добавить задачу с future](#submit--добавить-задачу-с-future)
  - [TaskOptions — параметры задачи](#taskoptions--параметры-задачи)
  - [Приоритеты](#приоритеты)
  - [Зависимости](#зависимости)
  - [Retry-политика](#retry-политика)
  - [cancel — отмена задачи](#cancel--отмена-задачи)
  - [wait / wait_result — ожидание результата](#wait--wait_result--ожидание-результата)
  - [wait_all — ожидание всех задач](#wait_all--ожидание-всех-задач)
  - [Группы (RAII)](#группы-raii)
  - [stats — статистика](#stats--статистика)
- [Для чего подходит](#для-чего-подходит)
- [Для чего НЕ подходит](#для-чего-не-подходит)
- [Ограничения](#ограничения)
- [Thread-safety и гарантии](#thread-safety-и-гарантии)
- [Расширенные примеры](#расширенные-примеры)
  - [Parallel map](#parallel-map)
  - [Пайплайн обработки данных](#пайплайн-обработки-данных)
  - [Retry с экспоненциальным backoff](#retry-с-экспоненциальным-backoff)
  - [Приоритетная очередь с мониторингом](#приоритетная-очередь-с-мониторингом)
  - [Группы и зависимости совместно](#группы-и-зависимости-совместно)
- [Типичные ошибки](#типичные-ошибки)
- [FAQ](#faq)
- [Внутреннее устройство](#внутреннее-устройство)
- [Запуск тестов](#запуск-тестов)
- [Changelog](#changelog)
- [Лицензия](#лицензия)

---

## Требования

| | |
|---|---|
| **Стандарт** | C++20 |
| **Компиляторы** | GCC 10+, Clang 12+, MSVC 19.29+ |
| **Зависимости** | только STL |
| **Тип** | header-only (`thread_pool.hpp`) |

---

## Установка

Скопируйте `thread_pool.hpp` в свой проект и подключите:

```cpp
#include "thread_pool.hpp"
```

При сборке добавьте флаг потоков:

```bash
# GCC / Clang
g++ -std=c++20 -pthread your_code.cpp

# MSVC
cl /std:c++20 your_code.cpp
```

---

## Быстрый старт

```cpp
#include "thread_pool.hpp"
#include <iostream>

int main() {
    // Создать пул с 4 воркерами
    auto pool = thread_pool::create(4);

    // 1. Простая задача через submit (типобезопасно, результат во future)
    auto fut = pool->submit([] { return std::string("hello"); });
    std::cout << fut.get() << "\n"; // "hello"

    // 2. Задача с id для последующего получения результата
    uint64_t id = pool->add_task([] { return 100; });
    int value;
    pool->wait_result(id, value); // value == 100

    // 3. Цепочка зависимостей
    uint64_t step1 = pool->add_task([] { return 10; });
    uint64_t step2 = pool->add_task(
        TaskOptions{.depends_on = {step1}},
        [] { return 20; }
    );
    pool->wait_result(step1, value); // получить step1
    pool->wait_result(step2, value); // получить step2 (запустится после step1)

    // 4. Группа задач (RAII — ждёт все задачи при выходе из scope)
    {
        auto group = pool->make_group();
        group.add_task([] { /* работа 1 */ });
        group.add_task([] { /* работа 2 */ });
    } // ← здесь блокируется до завершения обеих задач

    return 0;
}
```

---

## Концепции

### Два API добавления задач

Пул предоставляет два независимых способа добавить задачу:

| | `add_task` | `submit` |
|---|---|---|
| Возвращает | `uint64_t` (task_id) | `std::future<T>` |
| Получение результата | `wait_result(id, value)` | `future.get()` |
| Отмена | `cancel(id)` | — (нет id) |
| Зависимости | `depends_on = {id1, id2}` | через opts (если нужен id) |
| Очистка записи | вручную (wait_result) | автоматически |
| Типобезопасность | через `std::any_cast` | полная (шаблонный T) |

**Правило выбора:** если нужен только результат — используйте `submit`. Если нужен контроль над lifecycle (отмена, зависимости, группы) — `add_task`.

### Жизненный цикл задачи

```
add_task()
    │
    ▼
[pending] ──── все deps завершены ────► [in_queue]
                                              │
                                        воркер берёт
                                              │
                                              ▼
                                         [running]
                                         /    |    \
                              успех   /   failed    \ cancelled
                               ▼       ▼              ▼
                         [completed] [failed]     [cancelled]
                                      │
                               retries_left > 0
                                      │
                                      ▼
                                  [in_queue]  (повторная попытка)
```

---

## API

### Создание пула

```cpp
auto pool = thread_pool::create(uint32_t n_workers);
```

Создаёт пул с `n_workers` рабочими потоками. Воркеры запускаются немедленно и ждут задач.

> Пул не копируется и не перемещается. Всегда работайте через `shared_ptr`.

---

### add_task — добавить задачу

```cpp
// С параметрами по умолчанию
uint64_t id = pool->add_task(callable, args...);

// С явными опциями
uint64_t id = pool->add_task(TaskOptions{...}, callable, args...);
```

Возвращает `task_id` — уникальный идентификатор задачи в рамках пула.

```cpp
// Примеры
uint64_t id1 = pool->add_task([] { return 42; });
uint64_t id2 = pool->add_task(process_data, buffer, size);
uint64_t id3 = pool->add_task(
    TaskOptions{
        .priority    = Priority::High,
        .max_retries = 3,
        .name        = "critical_step"
    },
    heavy_computation, input
);
```

**Важно:** записи задач остаются в памяти до вызова `wait_result(id)` или `wait_all(true)`. При использовании `add_task` всегда вызывайте `wait_result` — иначе произойдёт утечка памяти.

---

### submit — добавить задачу с future

```cpp
[[nodiscard]]
std::future<T> fut = pool->submit(callable, args...);

[[nodiscard]]
std::future<T> fut = pool->submit(TaskOptions{...}, callable, args...);
```

Возвращает `std::future<T>`. Запись задачи в пуле удаляется автоматически после выполнения. `task_id` не возвращается.

```cpp
// Простое использование
auto fut = pool->submit([] { return 42; });
int x = fut.get(); // блокирует до готовности

// С аргументами
auto fut2 = pool->submit([](std::string s) { return s.size(); }, "hello");
size_t len = fut2.get();

// Исключения задачи попадают в future
auto fut3 = pool->submit([] -> int { throw std::runtime_error("oops"); });
try {
    fut3.get();
} catch (const std::runtime_error& e) {
    // поймает "oops"
}

// С приоритетом
auto fut4 = pool->submit(
    TaskOptions{.priority = Priority::Critical},
    urgent_task
);
```

> `[[nodiscard]]`: если вы не используете возвращённый `future`, компилятор выдаст предупреждение. Игнорирование `future` означает потерю исключений задачи.

---

### TaskOptions — параметры задачи

```cpp
struct TaskOptions {
    Priority              priority    = Priority::Normal;
    uint32_t              max_retries = 0;
    std::vector<uint64_t> depends_on  = {};
    std::string           name        = {};
    uint64_t              group_id    = 0;
};
```

| Поле | Тип | По умолчанию | Описание |
|---|---|---|---|
| `priority` | `Priority` | `Normal` | Приоритет в очереди |
| `max_retries` | `uint32_t` | `0` | Сколько раз повторить при исключении |
| `depends_on` | `vector<uint64_t>` | `{}` | task_id, которые должны завершиться до запуска |
| `name` | `string` | `""` | Имя для отладки (видно в stats) |
| `group_id` | `uint64_t` | `0` | Группа; обычно устанавливается через `GroupHandle::add_task` |

```cpp
// Designated initialization (C++20)
uint64_t id = pool->add_task(
    TaskOptions{
        .priority    = Priority::High,
        .max_retries = 2,
        .depends_on  = {prev_id},
        .name        = "encode_frame"
    },
    encode, frame
);
```

---

### Приоритеты

```cpp
enum class Priority : int {
    Low      = 0,
    Normal   = 1,  // по умолчанию
    High     = 2,
    Critical = 3
};
```

Задачи с более высоким приоритетом выбираются раньше. При равном приоритете — задача с меньшим `task_id` (поступившая раньше) имеет преимущество.

> **Внимание:** если постоянно добавлять задачи `Critical`, задачи `Low` могут голодать (starvation). Используйте `Critical` только для действительно срочных операций.

---

### Зависимости

```cpp
uint64_t fetch  = pool->add_task(fetch_data, url);
uint64_t parse  = pool->add_task(TaskOptions{.depends_on = {fetch}},  parse_data);
uint64_t render = pool->add_task(TaskOptions{.depends_on = {parse}},  render_view);
uint64_t save   = pool->add_task(TaskOptions{.depends_on = {fetch, parse}}, save_result);
```

- Задача переходит в `in_queue` только когда все её зависимости достигли terminal-статуса.
- Зависимости от уже завершённых задач игнорируются (считаются выполненными).
- Нельзя зависеть от задачи, запись которой уже удалена (`wait_result` был вызван). Это бросит `std::invalid_argument`.
- **Каскадной отмены нет:** `cancel(A)` не отменяет задачи, зависящие от A. Они будут запущены, когда A "разрешится". Для каскадной отмены отменяйте зависимые задачи вручную.

---

### Retry-политика

```cpp
uint64_t id = pool->add_task(
    TaskOptions{.max_retries = 3},
    unreliable_network_call
);
```

Если задача бросает исключение и `max_retries > 0`, она автоматически возвращается в очередь с тем же приоритетом. После исчерпания попыток переходит в статус `failed`, а исключение сохраняется и будет переброшено в `wait_result`.

```
Попытка 1: исключение → max_retries=3, retries_left=2 → обратно в очередь
Попытка 2: исключение → retries_left=1 → обратно в очередь
Попытка 3: исключение → retries_left=0 → статус failed, исключение сохранено
Попытка 4: успех      → статус completed
```

> Retry не добавляет задержки между попытками. Для экспоненциального backoff — реализуйте его внутри задачи.

---

### cancel — отмена задачи

```cpp
bool cancelled = pool->cancel(task_id);
```

| Возвращает | Значение |
|---|---|
| `true` | Задача успешно отменена |
| `false` | Задача уже `running` или в terminal-статусе — отмена невозможна |

```cpp
uint64_t id = pool->add_task(heavy_task);

if (!pool->cancel(id)) {
    // задача уже запущена — ждём завершения
    pool->wait(id);
}
```

**Особенности:**
- `cancel` работает только до начала выполнения. Running задачу прервать нельзя.
- Отменённая задача освобождает зависимые задачи (они перейдут в `in_queue`).
- Каскадной отмены зависимых задач нет.

---

### wait / wait_result — ожидание результата

```cpp
// Ждать завершения (результат не нужен)
pool->wait(task_id);

// Получить результат через std::any
std::any any_result = pool->wait_result(task_id);

// Получить типизированный результат
int value;
pool->wait_result(task_id, value);
```

- Блокирует вызывающий поток до terminal-статуса задачи.
- После возврата запись задачи удаляется из пула.
- **Повторный вызов с тем же id бросает исключение** (`already consumed`).
- Если задача завершилась с исключением (`failed`) — оно будет переброшено в `wait_result`.
- Если задача отменена (`cancelled`) — бросает `std::runtime_error("task cancelled")`.

```cpp
uint64_t id = pool->add_task([]() -> int {
    throw std::logic_error("bad input");
});

try {
    int res;
    pool->wait_result(id, res);
} catch (const std::logic_error& e) {
    // поймает оригинальное исключение задачи
}
```

---

### wait_all — ожидание всех задач

```cpp
pool->wait_all();        // ждать все задачи, добавленные до вызова
pool->wait_all(true);    // то же + очистить все записи из infos_
```

Блокирует поток до тех пор, пока суммарное количество завершённых задач (completed + failed + cancelled) не достигнет снапшота счётчика на момент вызова.

> **После `wait_all(true)`** все ранее полученные `task_id` становятся невалидными. Вызов `wait_result` по ним бросит `std::invalid_argument`.

```cpp
for (int i = 0; i < 100; i++)
    pool->add_task(process, data[i]);

pool->wait_all();        // дождаться всех 100 задач
pool->stats().print();   // статистика после завершения
```

---

### Группы (RAII)

Группы позволяют добавить набор задач и дождаться их всех единым блоком через RAII.

```cpp
{
    auto group = pool->make_group();

    group.add_task(task_a);
    group.add_task(task_b);
    group.add_task(TaskOptions{.priority = Priority::High}, task_c);

} // ← ~GroupHandle() вызывает wait() — блокирует до завершения всех трёх
```

**Явный wait:**

```cpp
auto group = pool->make_group();
group.add_task(long_job);
// ... другой код ...
group.wait(); // явно ждём
```

**Перемещение:**

```cpp
auto g1 = pool->make_group();
auto g2 = std::move(g1); // g1 теперь "пустой" (не вызовет wait при разрушении)
g2.add_task(job);
// ~g2: wait()
```

**Добавление задач с зависимостями внутри группы:**

```cpp
auto group = pool->make_group();
uint64_t id = group.add_task(fetch);
group.add_task(TaskOptions{.depends_on = {id}}, process);
// при выходе: ждёт fetch и process
```

---

### stats — статистика

```cpp
PoolStats s = pool->stats();
s.print(); // вывод в std::cout

// или вручную
std::cout << "completed: " << s.total_completed << "\n";
std::cout << "active:    " << s.active_workers   << "\n";
```

```
─── PoolStats ───────────────────
  ready_queue : 3       // задач ждут запуска
  pending_deps: 1       // задач ждут зависимостей
  active      : 2       // воркеров работают прямо сейчас
  submitted   : 100     // всего добавлено задач
  completed   : 94      // завершено успешно
  failed      : 2       // завершено с ошибкой
  cancelled   : 1       // отменено
─────────────────────────────────
```

| Поле | Описание |
|---|---|
| `ready_queue_size` | Задач в очереди (готовы, ждут свободного воркера) |
| `pending_deps_count` | Задач, ожидающих зависимостей |
| `active_workers` | Воркеров, выполняющих задачу прямо сейчас (±1 в момент снапшота) |
| `total_submitted` | Всего добавлено задач с момента создания пула |
| `total_completed` | Завершено успешно |
| `total_failed` | Завершено с исключением (после исчерпания retry) |
| `total_cancelled` | Отменено (cancel() или shutdown) |

---

## Для чего подходит

**Параллельная обработка независимых задач**
```cpp
for (auto& chunk : data_chunks)
    pool->add_task(process_chunk, chunk);
pool->wait_all();
```

**Пайплайны с зависимостями (DAG)**
```cpp
uint64_t load   = pool->add_task(load_from_disk, path);
uint64_t decode = pool->add_task(TaskOptions{.depends_on={load}},   decode_image);
uint64_t resize = pool->add_task(TaskOptions{.depends_on={decode}}, resize_image);
uint64_t save   = pool->add_task(TaskOptions{.depends_on={resize}}, save_result);
```

**CPU-bound вычисления**
Компрессия, криптография, матричные операции, рендеринг, симуляции — всё, что хорошо масштабируется на несколько ядер.

**Fire-and-forget с обработкой ошибок**
```cpp
auto fut = pool->submit(risky_operation, input);
try { fut.get(); } catch(...) { /* обработка */ }
```

**Фоновые задачи с приоритетами**
```cpp
pool->add_task(TaskOptions{.priority=Priority::Low}, background_index);
pool->add_task(TaskOptions{.priority=Priority::Critical}, urgent_request);
```

**Задачи с автоматическим повтором при сбое**
```cpp
pool->add_task(TaskOptions{.max_retries=5}, flaky_network_call);
```

---

## Для чего НЕ подходит

**Долгие блокирующие I/O операции**

Синхронное чтение файлов, ожидание сокета, `sleep` — каждая такая задача блокирует воркера целиком. При 4 воркерах достаточно 4 таких задач, чтобы пул перестал обрабатывать что-либо ещё. Используйте асинхронный I/O (`asio`, `io_uring`, `libuv`) или выделенный пул с бо́льшим числом потоков.

**Задачи, требующие прерывания изнутри**

`cancel()` предотвращает *запуск* задачи, но не прерывает уже выполняющуюся. Для cooperative cancellation реализуйте внутри задачи проверку флага:
```cpp
std::atomic<bool> stop_flag{false};
pool->add_task([&stop_flag]() {
    for (...) {
        if (stop_flag.load()) return; // сами проверяем флаг
        heavy_work_step();
    }
});
```

**Задачи с требованиями к конкретному потоку (thread-local storage, affinity)**

Пул не гарантирует выполнение на конкретном ядре или потоке. Для OpenGL-контекста, TLS-данных или CPU affinity нужен выделенный поток.

**Move-only типы результата через add_task**

`add_task` хранит результат в `std::any`, который плохо совместим с некоторыми move-only типами. Для `std::unique_ptr`, `std::promise` и подобных используйте `submit` — `std::future` корректно работает с move-only.

```cpp
// Плохо
uint64_t id = pool->add_task([] { return std::make_unique<Foo>(); });

// Хорошо
auto fut = pool->submit([] { return std::make_unique<Foo>(); });
auto ptr = fut.get();
```

**Real-time системы с жёсткими требованиями к задержкам**

`mutex + condvar + priority_queue` дают переменные задержки и возможные contention. Для жёсткого real-time нужны специализированные lock-free структуры и RT-ядро.

**Задачи с захватом ссылок на стек**

```cpp
// ОПАСНО — UB если функция завершится до задачи
void foo() {
    int local = 42;
    pool->add_task([&local]() { return local * 2; }); // dangling ref!
}

// Безопасно — копирование или shared_ptr
pool->add_task([local = 42]() { return local * 2; });
pool->add_task([data = std::make_shared<Data>(data)]() { return data->process(); });
```

---

## Ограничения

| Ограничение | Описание |
|---|---|
| Нет динамического изменения числа воркеров | Количество потоков фиксируется при создании. Нет `resize()`. |
| Нет fairness / aging | Задачи `Low` могут голодать, если постоянно добавляются `Critical`. |
| Нет таймаутов | Нет `wait_for(id, duration)`. Реализуйте через `std::future::wait_for` в `submit`. |
| Нет каскадной отмены | `cancel(A)` не отменяет задачи, зависящие от A. |
| Нет зависимостей на consumed-задачи | После вызова `wait_result(id)` зависеть от этого id нельзя. |
| Один мьютекс на всё | Высокое количество задач в секунду (~100k+/с) создаст contention. Для экстремальной нагрузки нужна lock-free очередь. |
| Нет планировщика с дедлайнами | Нет `earliest_deadline_first`. Только статические приоритеты. |
| Исключения в submit уходят в future | `run()` не видит `failed`-статуса для `submit`-задач — счётчик `stat_failed_` не увеличивается. |

---

## Thread-safety и гарантии

- **Все публичные методы** (`add_task`, `submit`, `cancel`, `wait`, `wait_result`, `wait_all`, `make_group`, `stats`) являются потокобезопасными и могут вызываться из любого потока одновременно.
- **Задача выполняется строго вне мьютекса** — пользовательский код не может вызвать дедлок через пул.
- **Shutdown** (`~thread_pool`): все незапущенные задачи отменяются, запущенные дожидаются завершения, воркеры корректно останавливаются.
- **Исключения в задачах** не роняют воркеров. Для `add_task` — сохраняются в `TaskInfo::error` и перебрасываются в `wait_result`. Для `submit` — уходят в `std::future`.
- **Double-wait защита**: повторный вызов `wait`/`wait_result` с одним id бросает `std::runtime_error`.

---

## Запуск тестов

```bash
g++ -std=c++20 -Wall -Wextra -pthread -fsanitize=thread -O1 -o test test.cpp
./test
```

Рекомендуется проверять с:
- `-fsanitize=thread` — гонки данных (TSan)
- `-fsanitize=address,undefined` — UB и утечки памяти (ASan/UBSan)

---

## Расширенные примеры

### Parallel map

Классический паттерн: применить функцию к каждому элементу коллекции параллельно и собрать результаты в том же порядке.

```cpp
#include "thread_pool.hpp"
#include <vector>
#include <numeric>

// Параллельный map: применяет fn к каждому элементу vec
template <typename T, typename Fn>
std::vector<std::invoke_result_t<Fn, T>>
parallel_map(std::shared_ptr<thread_pool> pool, const std::vector<T>& vec, Fn fn) {
    using R = std::invoke_result_t<Fn, T>;

    std::vector<std::future<R>> futures;
    futures.reserve(vec.size());

    for (const auto& item : vec)
        futures.push_back(pool->submit(fn, item));

    std::vector<R> results;
    results.reserve(vec.size());
    for (auto& f : futures)
        results.push_back(f.get()); // порядок сохранён

    return results;
}

// Использование
int main() {
    auto pool = thread_pool::create(std::thread::hardware_concurrency());

    std::vector<int> input(100);
    std::iota(input.begin(), input.end(), 1); // 1, 2, ..., 100

    auto squares = parallel_map(pool, input, [](int x) { return x * x; });
    // squares[0] == 1, squares[99] == 10000
}
```

---

### Пайплайн обработки данных

Моделирует многоступенчатый DAG: каждый шаг зависит от предыдущего, некоторые шаги выполняются параллельно.

```cpp
//
//   load ──► decode ──► [resize, analyze] ──► merge ──► save
//

auto pool = thread_pool::create(4);

// Шаг 1: загрузка
uint64_t load = pool->add_task(
    TaskOptions{.priority = Priority::High, .name = "load"},
    [](std::string path) { return read_file(path); },
    "/data/image.png"
);

// Шаг 2: декодирование (ждёт load)
uint64_t decode = pool->add_task(
    TaskOptions{.depends_on = {load}, .name = "decode"},
    decode_png
);

// Шаг 3a и 3b: параллельные операции (ждут decode)
uint64_t resize = pool->add_task(
    TaskOptions{.depends_on = {decode}, .name = "resize"},
    resize_to_thumbnail
);
uint64_t analyze = pool->add_task(
    TaskOptions{.depends_on = {decode}, .name = "analyze"},
    run_classifier
);

// Шаг 4: объединение (ждёт оба шага 3)
uint64_t merge = pool->add_task(
    TaskOptions{.depends_on = {resize, analyze}, .name = "merge"},
    merge_results
);

// Шаг 5: сохранение
uint64_t save = pool->add_task(
    TaskOptions{.depends_on = {merge}, .name = "save"},
    write_output, "/out/result.png"
);

// Ждём только финальный шаг — предыдущие завершатся автоматически
pool->wait(save);
```

---

### Retry с экспоненциальным backoff

`max_retries` повторяет задачу немедленно. Для backoff — реализуйте задержку внутри самой задачи:

```cpp
auto pool = thread_pool::create(4);

// Внутренний backoff: задача сама контролирует паузы между попытками
std::atomic<int> attempt_counter{0};

auto fut = pool->submit([&attempt_counter]() -> std::string {
    int attempt = attempt_counter.fetch_add(1);

    // Экспоненциальная пауза перед попыткой (кроме первой)
    if (attempt > 0) {
        auto delay = std::chrono::milliseconds(100 * (1 << std::min(attempt - 1, 5)));
        std::this_thread::sleep_for(delay);
        // ^ пауза: 100ms, 200ms, 400ms, 800ms, 1600ms, 3200ms
    }

    // Имитация ненадёжного сетевого вызова
    if (attempt < 3)
        throw std::runtime_error("connection refused");

    return "OK";
});

// Для задержки + max_retries вместе — оберните в одну задачу:
uint64_t id = pool->add_task(
    TaskOptions{.max_retries = 5, .name = "flaky_rpc"},
    [counter = std::make_shared<std::atomic<int>>(0)]() {
        int n = counter->fetch_add(1);
        if (n > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(50 << std::min(n, 4)));
        return call_remote_service();
    }
);
```

---

### Приоритетная очередь с мониторингом

Паттерн "сервер запросов" с разными приоритетами и периодическим выводом статистики:

```cpp
auto pool = thread_pool::create(std::thread::hardware_concurrency());

// Фоновый поток мониторинга
std::atomic<bool> running{true};
std::thread monitor([&] {
    while (running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        pool->stats().print();
    }
});

// Симуляция потока запросов с разными приоритетами
for (int i = 0; i < 1000; i++) {
    Priority prio;
    if      (i % 50 == 0) prio = Priority::Critical; // 2%  критические
    else if (i % 10 == 0) prio = Priority::High;      // 8%  высокий
    else if (i % 3  == 0) prio = Priority::Low;       // 30% фоновые
    else                  prio = Priority::Normal;     // 60% обычные

    pool->add_task(
        TaskOptions{.priority = prio, .name = "req_" + std::to_string(i)},
        handle_request, make_request(i)
    );
}

pool->wait_all();
running.store(false);
monitor.join();
```

---

### Группы и зависимости совместно

Группы и зависимости можно комбинировать: задачи внутри группы могут зависеть друг от друга.

```cpp
auto pool = thread_pool::create(4);

{
    auto group = pool->make_group();

    // Первая волна: три независимые задачи
    uint64_t t1 = group.add_task(TaskOptions{.name="fetch_users"},    fetch_users);
    uint64_t t2 = group.add_task(TaskOptions{.name="fetch_products"}, fetch_products);
    uint64_t t3 = group.add_task(TaskOptions{.name="fetch_prices"},   fetch_prices);

    // Вторая волна: зависит от первой
    uint64_t t4 = group.add_task(
        TaskOptions{.depends_on={t1, t2}, .name="build_catalog"},
        build_catalog
    );
    uint64_t t5 = group.add_task(
        TaskOptions{.depends_on={t2, t3}, .name="apply_pricing"},
        apply_pricing
    );

    // Финальный шаг: зависит от обеих задач второй волны
    group.add_task(
        TaskOptions{.depends_on={t4, t5}, .name="render_page"},
        render_page
    );

    // ~GroupHandle: ждёт все задачи группы, включая render_page

} // ← выход из scope = все 6 задач завершены

// Гарантия: страница отрендерена — можно отдавать ответ
send_response();
```

---

## Типичные ошибки

### ❌ Зависимость на consumed-задачу

```cpp
uint64_t a = pool->add_task([] { return 1; });
int val;
pool->wait_result(a, val); // ← запись a удалена

// ОШИБКА: std::invalid_argument
uint64_t b = pool->add_task(TaskOptions{.depends_on = {a}}, work);
```

**Решение:** если нужна зависимость, не вызывайте `wait_result(a)` до создания зависимой задачи. Либо используйте `calculated(a)` (не потребляет запись) перед принятием решения.

---

### ❌ Утечка памяти через add_task без wait_result

```cpp
for (int i = 0; i < 1000000; i++)
    pool->add_task([] { /* что-то делаем */ }); // ← id игнорируется

// Записи infos_ растут неограниченно, т.к. wait_result никогда не вызывается
```

**Решение:** используйте `submit` для fire-and-forget задач — записи удаляются автоматически. Либо периодически вызывайте `wait_all(true)`.

```cpp
// Правильно
for (int i = 0; i < 1000000; i++)
    pool->submit([] { /* что-то делаем */ }); // auto_cleanup внутри
```

---

### ❌ Захват ссылки на локальную переменную

```cpp
void process_request(Request req) {
    auto pool = get_shared_pool();
    int result = 0;

    // ОПАСНО: result может быть уничтожен до выполнения задачи
    pool->add_task([&result]() { result = heavy_compute(); });

    // Если функция вернётся раньше — UB
}
```

**Решение:** захватывать по значению или использовать `shared_ptr`:

```cpp
void process_request(Request req) {
    auto pool = get_shared_pool();
    auto result = std::make_shared<int>(0);

    uint64_t id = pool->add_task([result]() { *result = heavy_compute(); });
    pool->wait(id); // или wait_result
}
```

---

### ❌ Дедлок: задача ждёт другую задачу изнутри пула

```cpp
auto pool = thread_pool::create(1); // только 1 воркер!

pool->add_task([&pool]() {
    // Воркер занят этой задачей и ждёт future...
    auto inner = pool->submit([] { return 42; });
    return inner.get(); // ← ДЕДЛОК: единственный воркер занят, inner некому выполнить
});
```

**Решение:** никогда не ждите внутри задачи результата другой задачи из того же пула, если свободных воркеров может не хватить. Используйте зависимости (`depends_on`) вместо блокирующего ожидания.

```cpp
// Правильно: chain через зависимости
uint64_t outer = pool->add_task(step_one);
uint64_t inner = pool->add_task(TaskOptions{.depends_on={outer}}, step_two);
pool->wait(inner);
```

---

### ❌ Игнорирование [[nodiscard]] на submit

```cpp
pool->submit(risky_operation); // предупреждение компилятора: result ignored
// Исключение внутри risky_operation будет потеряно навсегда
```

**Решение:** всегда сохраняйте future:

```cpp
auto fut = pool->submit(risky_operation);
// позже:
try { fut.get(); } catch (const std::exception& e) { log(e.what()); }
// или немедленно, если результат не нужен но ошибки важны:
pool->submit(risky_operation).get(); // ждём и пробрасываем исключение
```

---

### ❌ Double-wait по одному task_id

```cpp
uint64_t id = pool->add_task([] { return 42; });

int a, b;
pool->wait_result(id, a); // OK
pool->wait_result(id, b); // ← std::runtime_error: already consumed
```

**Решение:** `wait_result` / `wait` — однократные операции. Для доступа к результату из нескольких мест — сохраните его до вызова `wait_result`:

```cpp
int result;
pool->wait_result(id, result);
// теперь result доступен сколько угодно раз
use(result);
use(result);
```

---

## FAQ

**Q: Сколько воркеров создавать?**

Зависит от типа задач:
- CPU-bound: `std::thread::hardware_concurrency()` — по одному потоку на ядро.
- Mixed (CPU + короткие блокировки): `hardware_concurrency() * 2`.
- Много мелких задач: `hardware_concurrency()` обычно оптимально.
- I/O bound: не используйте этот пул для I/O; или создайте отдельный пул с 4× воркеров.

```cpp
auto pool = thread_pool::create(std::thread::hardware_concurrency());
```

---

**Q: Можно ли один и тот же `thread_pool` использовать из нескольких потоков одновременно?**

Да. Все публичные методы потокобезопасны и защищены внутренним мьютексом.

---

**Q: Что произойдёт, если задача бросает исключение и `max_retries = 0`?**

Задача переходит в статус `failed`. Исключение сохраняется. При вызове `wait_result(id)` оно будет переброшено вызывающему. Для `submit` — уйдёт в `future.get()`.

---

**Q: Можно ли добавлять задачи из самой задачи (рекурсивно)?**

Да, но осторожно:

```cpp
pool->add_task([&pool]() {
    // Добавляем дочернюю задачу — это безопасно
    pool->add_task(child_work);
    // НЕ ждите её результата изнутри (см. ошибку про дедлок выше)
});
```

Рекурсивное добавление без ожидания — безопасно. Ожидание результата дочерней задачи изнутри пула — потенциальный дедлок.

---

**Q: Что происходит с задачами при уничтожении пула?**

Деструктор `~thread_pool`:
1. Переводит пул в `draining`-режим.
2. Отменяет все задачи из `pending_` (ещё ждут зависимостей).
3. Отменяет все задачи из `rq_` (готовы, но не запущены).
4. Ждёт завершения всех уже `running` задач.
5. Присоединяет (`join`) все рабочие потоки.

Задачи, попавшие в `running` до деструктора, **будут выполнены до конца**.

---

**Q: Можно ли передавать move-only объекты в задачу?**

Да, аргументы задачи. Нельзя — move-only **возвращаемые** типы через `add_task` (из-за `std::any`). Используйте `submit`:

```cpp
// Аргументы — можно
auto data = std::make_unique<BigData>();
pool->submit([](std::unique_ptr<BigData> d) { d->process(); }, std::move(data));

// Возвращаемое значение — только через submit
auto fut = pool->submit([] { return std::make_unique<Result>(); });
auto result = fut.get(); // std::unique_ptr<Result>
```

---

**Q: Как узнать, сколько задач ещё не завершено?**

```cpp
auto s = pool->stats();
uint64_t in_flight = s.ready_queue_size
                   + s.pending_deps_count
                   + s.active_workers;
```

Или через `wait_all()` — она блокирует до полного завершения.

---

**Q: Безопасно ли уничтожать `thread_pool` пока задачи ещё выполняются?**

Да. Деструктор корректно останавливает пул: незапущенные отменяются, running — дожидаются. Главное — чтобы задачи не обращались к уже уничтоженным данным (например, если задача захватила `this` объекта, который был уничтожен).

---

**Q: Почему `total_submitted` в stats равно следующему свободному id, а не числу задач?**

Поле отражает `last_id_` — монотонный счётчик, который только растёт. При 5 задачах он равен 5 (id: 0,1,2,3,4). Это точно соответствует числу задач, добавленных с момента создания пула.

---

## Внутреннее устройство

### Структуры данных

```
thread_pool
│
├── rq_           std::priority_queue<ReadyEntry>
│                 Задачи, готовые к немедленному запуску.
│                 Упорядочены по (Priority DESC, task_id ASC).
│
├── pending_      unordered_map<uint64_t, PendingEntry>
│                 Задачи, ожидающие зависимостей.
│                 Ключ: task_id.
│
├── dep_waiters_  unordered_map<uint64_t, vector<uint64_t>>
│                 Обратный индекс зависимостей.
│                 dep_id → [task_id, которые ждут dep_id].
│                 Позволяет resolve_locked работать за O(waiters),
│                 а не O(all pending).
│
├── infos_        unordered_map<uint64_t, TaskInfo>
│                 Метаданные всех живых задач: статус, результат,
│                 исключение, имя, флаги.
│
└── groups_       unordered_map<uint64_t, GroupInfo>
                  Активные группы: pending_ids + sealed-флаг.
```

### Алгоритм воркера (`run()`)

```
loop:
  1. cv_.wait(lock) — ждёт rq_ непустой или state != running
  2. if rq_.empty(): выход из цикла (shutdown)
  3. rq_.top() / rq_.pop() → ReadyEntry e
  4. if infos_[e.task_id] отсутствует или cancelled → cv_.notify_all(), continue
  5. status = running; ++active_; unlock мьютекса
  6. result = (*e.task)()         ← ВЫПОЛНЕНИЕ ВНЕ МЬЮТЕКСА
  7. --active_; lock мьютекса
  8. if exception && retries_left > 0:
       status = in_queue; push обратно в rq_
     else:
       status = completed/failed; сохранить result/exception
       resolve_locked(tid)        ← разбудить зависимые задачи
       group_finish_locked(...)   ← уведомить группу
       if auto_cleanup && !consumed: infos_.erase(tid)
       cv_.notify_all()
```

### Алгоритм зависимостей

При добавлении задачи B с `depends_on = {A}`:
- Если A уже terminal — B сразу попадает в `rq_`.
- Иначе — B записывается в `pending_`, а `dep_waiters_[A]` получает id B.

Когда A завершается (`resolve_locked(A)`):
- Итерируемся по `dep_waiters_[A]`.
- Для каждого ожидальщика удаляем A из `remaining_deps`.
- Если `remaining_deps` опустел — перемещаем задачу из `pending_` в `rq_`.
- Удаляем `dep_waiters_[A]`.

### Lazy-cancel в rq_

Отменённые задачи не удаляются из `std::priority_queue` сразу (это O(n)). Вместо этого воркер при извлечении проверяет статус: если `cancelled` — пропускает. Это safe и simple, но неэффективно при массовых отменах (воркеры "прокручивают" отменённые записи). Для систем с тысячами отмен в секунду рассмотрите сегментированную очередь с поддержкой удаления.

### Почему один мьютекс?

Простота и корректность. Пул ориентирован на задачи длиннее ~1 мкс — при таком соотношении накладные расходы на мьютекс (сотни наносекунд) незначительны. Для задач-"микробатчей" (<1 мкс) нужна lock-free очередь (MPMC), но это существенно усложняет код.

---

## Запуск тестов

```bash
g++ -std=c++20 -Wall -Wextra -pthread -fsanitize=thread -O1 -o test test.cpp
./test
```

Рекомендуется проверять с:
- `-fsanitize=thread` — гонки данных (TSan)
- `-fsanitize=address,undefined` — UB и утечки памяти (ASan/UBSan)

---

## Changelog
v1.4 (текущая)
+Критические исправления (перенесены из v1.3)
[FIX-1] wait(), wait_result(), wait_all(), GroupHandle::wait() — обнаруживают вызов из воркер-потока пула и немедленно бросают `std::logic_error` вместо thread-starvation deadlock.
[FIX-2] Деструктор `~thread_pool` — при отмене задач из `rq_` теперь вызывает `resolve_locked()` и `group_finish_locked()`. До исправления `GroupInfo::pending_ids` не обнулялся → `wait_group_impl` мог зависнуть после начала shutdown.
[FIX-3] `cancel()` — не отменяет задачу если `consumed=true` (кто-то уже заблокирован в `wait_result()`). Раньше `cancel()` выигрывал гонку и `wait_result` бросал "task cancelled" вместо реального результата.

Защита от ошибок пользователя

[P1] `create(0)` → `std::invalid_argument` немедленно. Раньше пул без воркеров создавался успешно, а `wait_all()` зависал навсегда.
[P2] Кольцевые зависимости (A→B→A) — детектируются BFS при добавлении задачи → `std::invalid_argument`. Самозависимость (A depends_on A) — частный случай.
[P3] `last_id_` стартует с `FIRST_TASK_ID=1`. `task_id=0` зарезервирован как `INVALID_TASK_ID` (аналог `nullptr`). `depends_on={0}` → `invalid_argument`.
[P4] `wait_result<T>` с неверным типом `T` → `std::runtime_error` с именами запрошенного и хранимого типов вместо загадочного `std::bad_any_cast`.
[P5] `GroupHandle::add_task()` и `GroupHandle::wait()` на moved-from или уже исчерпанном handle → `std::logic_error` вместо UB / разыменования nullptr.
[P7] `max_retries > RETRIES_HARD_LIMIT(255)` → `std::invalid_argument`. Предотвращает случайный бесконечный retry из-за опечатки.
[P8] `TaskOptions::validate()` — явная проверка опций до захвата мьютекса. Ошибка диагностируется раньше и с понятным сообщением.

Исправления, выявленные тестами

[B1] `cancel()` — устранён двойной `infos_.find(id)` (артефакт слияния патчей).
[B2] `check_cycle_locked` явно перемещён в секцию `private`.
[B3] `wait_all()` защищён `assert_not_worker_locked` — аналогично `wait()`/`wait_result()`.
[B4] `wait_all(cleanup=true)` — кумулятивные счётчики `stat_*` не обнуляются; вводится базис (`stat_*_base_`, `last_id_base_`). Следующий `wait_all()` корректно ждёт только задачи, добавленные после `cleanup`.
[B5] `stats().total_submitted` теперь корректен после `cleanup`: `last_id_ - FIRST_TASK_ID` вместо сырого `last_id_`.

Удалено

[P6] Флаг `cleaned_up_` и блокировка `add_task` после `wait_all(cleanup=true)` — оказались слишком агрессивными и ломали легальные сценарии повторного использования пула. Реальную защиту обеспечивает существующая проверка `infos_.count(dep)` в фазе валидации.


v1.3

[FIX-1] Deadlock: `wait*()`/`wait_group_impl()` из воркер-потока → `std::logic_error`. Поле `worker_ids_` хранит id всех воркеров.
[FIX-2] Деструктор: `resolve_locked()` + `group_finish_locked()` при отмене задач из `rq_`.
[FIX-3] `cancel()`: не отменяет `consumed`-задачи.
### v1.2 

**Исправлены баги:**

- `GroupHandle(GroupHandle&&)` — move-конструктор теперь устанавливает `done_=true` в moved-from объекте. Раньше деструктор moved-from объекта вызывал `wait()` через нулевой `pool_` → crash.
- `auto_cleanup=true` + `wait()`/`wait_result()` — предикат `cv_.wait` использует `find` вместо `at`. Раньше `std::out_of_range` внутри предиката вызывал `std::terminate`.
- `auto_cleanup` убран из публичного `TaskOptions` — стал внутренним параметром `enqueue_locked`. Пользователь не может выставить его случайно и сломать пул.
- `cancel()` теперь удаляет отменяемую задачу из `dep_waiters_` — нет накопления stale-записей при массовых отменах.
- `submit(F, Args)` делегирует в `submit(TaskOptions, F, Args)` — единственный code-path, нет дублирования.
- `wait_result` при исчезнувшей записи теперь бросает исключение вместо тихого возврата пустого `std::any{}`.
- Введён `enqueue_locked`: валидация (`group_id`, `dep` ids) выполняется до первой мутации состояния — exception safety.
- `wait_group_impl` использует `at()` вместо `operator[]` — нет молчаливого создания записи при несуществующем gid.
- `run()`: dangling reference после `infos_.erase(tid)` устранён.
- `wait_all()`: снапшот `last_id_` берётся под мьютексом — нет гонки с параллельными `add_task`.
- `~thread_pool`: задачи из `rq_` теперь также отменяются при shutdown (раньше только `pending_`).

**Улучшения:**

- `[[nodiscard]]` на оба `submit`.
- Полные doc-комментарии на все публичные методы и структуры.
- `#pragma once`.

---

### v1.1

**Исправлены баги:**

- `GroupHandle` — добавлен move-constructor (частично, финально исправлен в v1.2).
- `resolve_locked` — переход на обратный индекс `dep_waiters_`: O(waiters) вместо O(all pending).
- `wait`/`wait_result` — защита от double-wait через флаг `consumed`.
- `run()` — `Task` хранится через `shared_ptr<Task>` (нет копирования `std::function` при push/pop).
- `submit` — добавлен `auto_cleanup` для автоматической очистки записей.

---

### v1.0

Первоначальный релиз. Базовый пул с приоритетами, зависимостями, retry, cancel, группами и статистикой.

---

## Лицензия

MIT
