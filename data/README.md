# data/

This directory is **gitignored** (see `.gitignore`) — only this README is tracked.
Market data is large and may be licensed, so it never enters version control.

## What goes here

Equities market-by-order (MBO) / limit-order-book data:

- **NASDAQ TotalView-ITCH** — raw exchange feed (`.itch` / `.gz`).
- **Databento MBO** — normalized market-by-order (`.dbn` / `.dbn.zst`).

## How to get it

1. **Databento** (recommended for getting started):
   - Sign up at <https://databento.com> and create an API key.
   - Pull a sample, e.g. one symbol / one session of `XNAS.ITCH` MBO.
   - Store the key in an env var (never commit it): `DATABENTO_API_KEY=...`.

2. **NASDAQ ITCH sample files**:
   - Historical ITCH 5.0 samples are published by NASDAQ; download and drop the
     `.gz` here.

## Layout (suggested)

```
data/
├── raw/        # untouched downloads (itch/dbn)
└── processed/  # parsed/normalized intermediates
```

Keep raw and processed separate so you can always re-derive processed from raw.
