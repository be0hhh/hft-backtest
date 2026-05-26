Понял: TradFi-активы, но торгуемые внутри криптобирж / crypto rails. Это действительно может быть интереснее, чем обычный BTC/ETH HFT, потому что рынок новый, фрагментированный, с разной ликвидностью, разными индексами, разными фидами и слабее выровненными ценами.

Мой вывод после поиска: самая перспективная зона — не классический HFT, а basis / lead-lag / fair-value trading между tokenized stocks, RWA perps и референсной TradFi-ценой.

Что сейчас существует

На криптобиржах уже есть несколько форм TradFi-экспозиции:

1. tokenized stocks / ETFs spot
2. stock futures / RWA index perpetuals
3. tokenized equity perpetual futures
4. onchain xStocks liquidity / AMM / lending
5. oracle-priced synthetic TradFi markets

Самый заметный стек сейчас — xStocks. Это токенизированные версии американских акций и ETF, например AAPLx, NVDAx, TSLAx, SPYx, QQQx. По документации xStocks, каждый токен даёт экономическую экспозицию к underlying equity и collateralized 1:1 соответствующим активом, но это не обычная акция с голосованием и полноценными shareholder rights.

Kraken уже предлагает xStocks, включая маржинальные xStocks вроде AAPLx, GLDx, GOOGLx, NVDAx, QQQx, SPYx, TSLAx с 3x leverage для некоторых регионов; при этом есть географические ограничения, включая недоступность в США, Австралии, Канаде и UK.

Bybit тоже даёт xStocks на spot market и прямо пишет, что xStocks торгуются 24/7, но комиссии для xStocks на Bybit на базовом уровне очень большие для HFT: 0.20% maker / 0.20% taker на VIP 0. Это сразу убивает мелкий intra-spread HFT, но не обязательно убивает большие дислокации.

Bitget пошёл в сторону RWA Index Perpetual Futures: stock-like perps на токенизированные акции вроде AAPL, GOOGL, AMZN, META, MCD. Важная деталь: эти контракты используют composite pricing из нескольких tokenized-stock issuers и работают по 5×24, с закрытием на выходные и праздники, frozen market price и disabled new orders во время closures.

То есть рынок уже не теоретический. Он существует, но он ещё кривой.

Где тут edge

Один лучший исследовательский путь:

TradFi reference price
→ fair value model
→ crypto exchange tokenized instrument
→ deviation / basis / lead-lag
→ trade only large statistically persistent dislocations

То есть не “предсказывать Apple”, а считать:

сколько должен стоить AAPLx / NVDAx / SPYx / stock-perp прямо сейчас

И сравнивать с тем, что даёт криптобиржа.

Формально:

fair_price =
    underlying_tradfi_price
    × FX_or_stablecoin_adjustment
    + dividend/corporate_action_adjustment
    + funding/carry_adjustment
    + issuer/redemption_premium
    + liquidity_premium

Потом:

basis = crypto_market_price - fair_price

И уже дальше:

если basis экстремальный,
держится дольше latency/noise,
переживает fees,
и mean-reverts статистически,
тогда это кандидат стратегии

Это не обязательно HFT в 100 микросекунд. Скорее fast statistical arbitrage на горизонтах от секунд до минут. И это лучше подходит под твой сетап.

Pyth vs dxFeed
Pyth

Pyth полезен как oracle/reference layer. В документации Pyth есть два режима: Pyth Pro для institutional / low-latency use cases и Pyth Core для DeFi/TradFi builders с deterministic on-chain delivery; для Core указана update frequency около 400ms.

Pyth feeds включают разные asset classes, включая crypto, US equities и metals; каждый feed имеет уникальный ID, а доступ к feed IDs идёт через Hermes API.

Для стратегии важны не только price, но и:

publish_time
confidence interval
best_bid
best_ask
ema_price
ema_confidence

Pyth сам подчёркивает, что confidence interval и best bid/ask — разные вещи: confidence показывает disagreement между publishers, а bid/ask нужен для spread/liquidity/execution planning.

Вывод по Pyth:

Pyth Core — хорошо для oracle-aware research / DeFi / sanity check.
Pyth Pro — потенциально интересно для low-latency reference.
Но Pyth не надо считать “биржевым tape feed” уровня прямого Nasdaq/CME.
dxFeed

dxFeed больше похож на нормальный market-data vendor для TradFi. Он даёт real-time и historical market data по equities, futures, options, ETFs, indices, FX, crypto, commodities, treasuries и т.д.; у них есть Java, C/C#, Python, FIX, REST и другие API.

Важнее всего: dxFeed пишет, что sourcing/storing идёт из direct market data feeds worldwide, данные доступны tick-by-tick, consolidated/normalized, с low-latency API.

Вывод по dxFeed:

для fair-value модели лучше dxFeed / direct TradFi feed;
для onchain/oracle reference — Pyth;
для торговли — order book конкретной криптобиржи.

То есть идеальная архитектура:

dxFeed / direct TradFi feed
    → true reference

Pyth
    → oracle/reference cross-check

crypto exchange WS
    → actual tradable market

backtest
    → basis / lead-lag / dislocation decay
Где именно может быть “золотая жила”

Я бы сфокусировался на tokenized equity perp/spot basis.

Почему именно это:

1. Новый рынок.
2. Фрагментированная ликвидность.
3. Разные venues используют разные индексные методологии.
4. Есть 24/7 crypto trading, но underlying TradFi имеет trading hours.
5. Есть фидовая асимметрия: TradFi reference, oracle, CEX index, onchain pool.
6. У многих участников нет нормальной fair-value модели.

Один конкретный пример:

NVDA реальная акция движется на Nasdaq.
NVDAx на crypto venue реагирует с задержкой / премией / дисконтом.
NVDA stock perp на другой crypto venue имеет funding и index mechanics.
Pyth показывает oracle aggregate.
dxFeed показывает референсный TradFi tape.

Ты строишь:

basis_nvda_xstock = NVDAx_mid - NVDA_reference_mid
basis_nvda_perp   = NVDA_perp_mark - NVDA_reference_mid
cross_basis       = NVDA_perp_mark - NVDAx_mid

Потом исследуешь:

basis mean reversion
basis half-life
basis volatility by session
basis during US market open
basis during after-hours
basis around earnings/news
basis across venues

Если находишь, что:

basis > threshold
→ через 5–60 секунд basis статистически схлопывается
→ после fees/slippage остаётся edge

тогда это уже потенциальная стратегия.

Что между биржами можно делать в теории

Главная конструкция:

same economic exposure, different venue

Например:

AAPLx on Kraken
AAPLx on Bybit
AAPL RWA perp on Bitget
AAPL oracle reference from Pyth
AAPL real-time reference from dxFeed

Теоретические типы сделок:

1. crypto venue A vs crypto venue B
2. tokenized spot vs RWA perp
3. crypto tokenized asset vs TradFi reference
4. onchain xStock pool vs CEX xStock order book
5. RWA perp mark/index vs external fair value

Но практически лучший старт — не пытаться сразу делать execution arbitrage. Сначала надо сделать basis recorder:

for each symbol:
    record tradfi_ref_bid_ask
    record pyth_price/conf/publish_time
    record cex_mid/bid/ask/depth
    record perp_mark/index/funding
    compute basis
    compute basis decay

Это даст ответ, есть ли вообще неэффективность.

Ключевой риск

Эти продукты не равны настоящим акциям.

xStocks — это tracker certificate / tokenized exposure. Они дают экономическую экспозицию, но не обычные shareholder rights; dividends отражаются через rebasing mechanism, а не как обычная cash dividend выплата держателю акции.

Также у xStocks есть compliance/control features. Например, Solana case study описывает Pausable Config и Permanent Delegate, который может иметь права transfer/burn в рамках lawful/compliance requirements.

Это значит:

basis может существовать не просто из-за inefficiency,
а из-за реального product/legal/custody/redemption risk.

То есть нельзя тупо считать:

AAPLx должен всегда равняться AAPL

Надо считать:

AAPLx = AAPL - product_risk_discount + liquidity_premium + redemption_basis
Что я бы делал в твоём проекте

Я бы добавил отдельный исследовательский модуль:

tradfi_basis_research/

И внутри сделал не стратегию, а recorder/analysis:

symbols:
    AAPLx
    NVDAx
    TSLAx
    SPYx
    QQQx
    GLDx

sources:
    crypto_exchange_orderbook
    crypto_exchange_trades
    perp_mark/index/funding
    pyth_price/conf/publish_time
    dxfeed_or_other_tradfi_reference

outputs:
    basis_timeseries
    basis_decay_report
    lead_lag_report
    dislocation_report
    simulated_pnl_report

Основная таблица:

timestamp
symbol
venue
crypto_bid
crypto_ask
crypto_mid
crypto_depth
tradfi_bid
tradfi_ask
tradfi_mid
pyth_price
pyth_conf
pyth_publish_time
perp_mark
perp_index
funding_rate
basis_bps
basis_zscore
basis_future_1s
basis_future_5s
basis_future_30s
basis_future_300s

Главная проверка:

E[-sign(basis) × future_basis_change | abs(basis_zscore) > k] > fees + slippage

То есть если токен сильно выше fair value, ты ожидаешь basis вниз. Если сильно ниже — basis вверх.

Как тестить

Тебе нужны не обычные свечки, а event-aligned joins.

Правильная логика:

1. Привести все источники к одной clock domain.
2. Для каждого crypto market event найти последний известный TradFi reference.
3. Отдельно хранить staleness:
       now - tradfi_ref_timestamp
       now - pyth_publish_time
4. Не использовать stale reference как live truth.
5. Строить basis только если reference свежий.

Метрики:

basis mean
basis std
basis skew
basis half-life
basis autocorrelation
basis crossing rate
basis decay after z-score event
PnL after cost buffer
hit rate by z-score bucket
capacity by top-of-book depth

Самый важный график:

abs(basis_zscore) bucket → future basis reversion

Хорошая картина:

zscore 1–2   → слабый reversion
zscore 2–3   → нормальный reversion
zscore 3–5   → сильный reversion
zscore > 5   → либо huge edge, либо product/news risk

Если такой монотонности нет — это не стратегия, а шум.

Реалистичный вывод

Да, TradFi-на-криптобиржах может быть лучше, чем обычный crypto HFT.

Но не как “быстрый арбитраж в стакане”, а как:

fair-value engine
+ multi-source price recorder
+ basis statistics
+ cross-venue comparison
+ selective execution only on large dislocations

Самая сильная идея:

tokenized equity / RWA perp basis trading

Схема:

dxFeed/direct TradFi reference
+ Pyth oracle sanity check
+ CEX xStock / RWA perp order books
→ detect basis
→ measure decay
→ trade only statistically proven dislocations

Это направление я бы поставил выше обычного spread_maker на BTC/ETH. Там ты воюешь с профессиональными HFT. Здесь рынок моложе, грязнее, менее стандартизирован, а значит шанс найти неэффективность выше.