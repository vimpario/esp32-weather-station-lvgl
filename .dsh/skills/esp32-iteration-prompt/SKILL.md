# Skill: esp32-iteration-prompt

## Назначение
Это шаблон поведения DeepSeek Harness при каждом итерационном изменении проекта.

## Рабочий цикл
```text
READ → PLAN → PATCH → BUILD → FLASH (when hardware needed) → TEST → REPORT
```

## Перед изменением
- Прочитать относящиеся `SKILL.md`.
- Проверить текущую структуру проекта.
- Найти существующие pin definitions, data models и UI state.
- Не создавать новые каталоги/файлы без явной необходимости и отдельного разрешения.

## После изменения
- Собрать проект через PlatformIO.
- Для UI отдельно проверить, что все четыре режима продолжают существовать.
- Для аппаратных изменений проверить соответствующий sensor/display path.
- Не переписывать рабочие части проекта ради косметики.

## Формат отчёта агента
```text
Changed:
- ...

Build:
- PASS/FAIL

Hardware test:
- NOT RUN / PASS / FAIL

Known risks:
- ...

Next smallest step:
- ...
```

## Guardrails
- Никаких секретов в git.
- Никаких длинных блокирующих `delay()` в основном цикле.
- Никаких внешних web-CDN зависимостей.
- Не менять pin mapping без явной причины.
- Сохранять разделение: sensor → data model → transport → presentation.
