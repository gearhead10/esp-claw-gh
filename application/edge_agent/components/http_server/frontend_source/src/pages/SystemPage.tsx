import {
  createEffect,
  createMemo,
  createSignal,
  For,
  onCleanup,
  Show,
  untrack,
  type Component,
} from 'solid-js';
import { fetchSystemTasks, type SystemSnapshot, type SystemTask } from '../api/client';
import { t } from '../i18n';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { SelectInput } from '../components/ui/FormField';
import { Banner } from '../components/ui/Banner';

/* Allowed polling intervals (seconds). Lower bound 2s keeps the ESP from
 * spending all its time serving HTTP polls; upper bound 120s is enough for
 * passive monitoring. Default 4s matches the user's preference. */
const REFRESH_INTERVALS_SEC = [2, 4, 8, 15, 30, 60, 120] as const;
const DEFAULT_INTERVAL_SEC = 4;

type TaskRow = SystemTask & { cpu_pct: number };

function formatBytes(n: number): string {
  if (!Number.isFinite(n) || n <= 0) return '0 B';
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / 1024 / 1024).toFixed(2)} MB`;
}

function formatDuration(ms: number): string {
  if (!Number.isFinite(ms) || ms <= 0) return '0s';
  const totalSec = Math.floor(ms / 1000);
  const sec = totalSec % 60;
  const min = Math.floor(totalSec / 60) % 60;
  const hour = Math.floor(totalSec / 3600) % 24;
  const day = Math.floor(totalSec / 86400);
  if (day > 0) return `${day}d ${hour}h ${min}m`;
  if (hour > 0) return `${hour}h ${min}m`;
  if (min > 0) return `${min}m ${sec}s`;
  return `${sec}s`;
}

const Stat: Component<{ label: string; value: string; sub?: string; warn?: boolean }> = (
  props,
) => (
  <div class="flex flex-col gap-1 px-3 py-2 rounded-[var(--radius-sm)] bg-white/[0.02] border border-[var(--color-border-subtle)]">
    <span class="text-[0.7rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
      {props.label}
    </span>
    <span
      class={[
        'text-[0.95rem] font-mono',
        props.warn ? 'text-[var(--color-orange)]' : 'text-[var(--color-text-primary)]',
      ].join(' ')}
    >
      {props.value}
    </span>
    <Show when={props.sub}>
      <span class="text-[0.7rem] text-[var(--color-text-muted)]">{props.sub}</span>
    </Show>
  </div>
);

const StateBadge: Component<{ state: string }> = (props) => {
  const styleMap: Record<string, string> = {
    running: 'bg-[var(--color-accent)]/20 text-[var(--color-accent)]',
    ready: 'bg-blue-500/20 text-blue-300',
    blocked: 'bg-amber-500/20 text-amber-300',
    suspended: 'bg-gray-500/20 text-gray-300',
    deleted: 'bg-red-500/20 text-red-300',
  };
  return (
    <span
      class={[
        'inline-flex px-1.5 py-0.5 rounded text-[0.7rem] font-mono',
        styleMap[props.state] ?? 'bg-white/5 text-[var(--color-text-muted)]',
      ].join(' ')}
    >
      {props.state}
    </span>
  );
};

/* Human-friendly mapping for esp_reset_reason() string returned by the backend.
 * Tag the entries that imply a fault so the UI can highlight them in orange. */
const RESET_REASON_LABEL: Record<string, { label: string; warn?: boolean }> = {
  poweron:   { label: 'Power on' },
  external:  { label: 'External reset' },
  software:  { label: 'Software reset' },
  panic:     { label: 'Panic (crash)', warn: true },
  int_wdt:   { label: 'Interrupt watchdog', warn: true },
  task_wdt:  { label: 'Task watchdog', warn: true },
  wdt:       { label: 'Watchdog', warn: true },
  deepsleep: { label: 'Wake from deep sleep' },
  brownout:  { label: 'Brownout', warn: true },
  sdio:      { label: 'SDIO host' },
  unknown:   { label: 'Unknown' },
};

export const SystemPage: Component = () => {
  const [snapshot, setSnapshot] = createSignal<SystemSnapshot | null>(null);
  const [prevSnapshot, setPrevSnapshot] = createSignal<SystemSnapshot | null>(null);
  const [intervalSec, setIntervalSec] = createSignal<number>(DEFAULT_INTERVAL_SEC);
  const [error, setError] = createSignal<string | null>(null);
  /* Wall-clock anchor at the moment of the latest poll, used to extrapolate
   * the device uptime client-side every second between polls. */
  const [snapshotAt, setSnapshotAt] = createSignal(0);
  const [tickNow, setTickNow] = createSignal(Date.now());
  let abort: AbortController | null = null;
  let timer: number | null = null;
  let uptimeTick: number | null = null;

  /* Use a plain ref instead of a tracked signal so reading it inside the
   * polling function doesn't make the interval-setup effect react to its
   * own writes (which would tear down the timer and re-fire instantly). */
  let isLoading = false;
  const pollOnce = async () => {
    if (isLoading) return;
    isLoading = true;
    abort?.abort();
    abort = new AbortController();
    try {
      const data = await fetchSystemTasks(abort.signal);
      setPrevSnapshot(untrack(snapshot));
      setSnapshot(data);
      setSnapshotAt(Date.now());
      setError(null);
    } catch (err) {
      if ((err as Error).name !== 'AbortError') {
        setError((err as Error).message);
      }
    } finally {
      isLoading = false;
    }
  };

  /* Only the interval signal should drive this effect. Wrap the immediate
   * poll in untrack so its synchronous signal reads do not become extra
   * dependencies. */
  createEffect(() => {
    const seconds = intervalSec();
    if (timer !== null) {
      window.clearInterval(timer);
    }
    untrack(() => {
      void pollOnce();
    });
    timer = window.setInterval(() => {
      void pollOnce();
    }, seconds * 1000);
  });

  /* Independent 1-second ticker so the Uptime stat advances live between
   * device polls. Cost is negligible (one signal update per second). */
  uptimeTick = window.setInterval(() => setTickNow(Date.now()), 1000);

  onCleanup(() => {
    if (timer !== null) window.clearInterval(timer);
    if (uptimeTick !== null) window.clearInterval(uptimeTick);
    abort?.abort();
  });

  const liveUptimeMs = createMemo(() => {
    const s = snapshot();
    if (!s) return 0;
    const elapsed = tickNow() - snapshotAt();
    return s.uptime_ms + (elapsed > 0 ? elapsed : 0);
  });

  const resetReasonInfo = createMemo(() => {
    const reason = snapshot()?.reset_reason;
    if (!reason) return { label: '-' };
    return RESET_REASON_LABEL[reason] ?? { label: reason };
  });

  const tasks = createMemo<TaskRow[]>(() => {
    const curr = snapshot();
    const prev = prevSnapshot();
    if (!curr) return [];
    const totalDelta =
      prev && curr.total_runtime_counter > prev.total_runtime_counter
        ? curr.total_runtime_counter - prev.total_runtime_counter
        : 0;
    const prevById = new Map<number, SystemTask>();
    prev?.tasks.forEach((tk) => prevById.set(tk.id, tk));

    return curr.tasks
      .map((tk) => {
        let cpu = 0;
        const p = prevById.get(tk.id);
        if (p && totalDelta > 0) {
          const td = tk.runtime_counter - p.runtime_counter;
          if (td > 0) cpu = (td / totalDelta) * 100;
        }
        return { ...tk, cpu_pct: cpu };
      })
      .sort((a, b) => b.cpu_pct - a.cpu_pct || a.name.localeCompare(b.name));
  });

  const psramPct = createMemo(() => {
    const s = snapshot();
    if (!s || !s.total_psram_bytes) return 0;
    return ((s.total_psram_bytes - s.free_psram_bytes) / s.total_psram_bytes) * 100;
  });
  const internalPct = createMemo(() => {
    const s = snapshot();
    if (!s || !s.total_internal_bytes) return 0;
    return ((s.total_internal_bytes - s.free_internal_bytes) / s.total_internal_bytes) * 100;
  });

  return (
    <TabShell>
      <PageHeader
        title={t('navSystem') as string}
        description={t('systemSubtitle') as string}
        actions={
          <div class="flex items-center gap-2">
            <span class="text-[0.78rem] text-[var(--color-text-muted)]">
              {t('systemRefreshLabel')}
            </span>
            <SelectInput
              value={String(intervalSec())}
              onChange={(event) => setIntervalSec(parseInt(event.currentTarget.value, 10))}
              fieldClass="w-32"
            >
              <For each={REFRESH_INTERVALS_SEC}>
                {(sec) => (
                  <option value={sec}>
                    {sec < 60 ? `${sec}s` : `${sec / 60}m`}
                  </option>
                )}
            </For>
            </SelectInput>
          </div>
        }
      />
      <Show when={error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('systemSectionOverview') as string}>
          <div class="grid gap-2 sm:grid-cols-2 md:grid-cols-4 pt-2">
            <Stat
              label={t('systemUptime') as string}
              value={snapshot() ? formatDuration(liveUptimeMs()) : '-'}
            />
            <Stat
              label={t('systemTaskCount') as string}
              value={snapshot() ? String(snapshot()!.task_count) : '-'}
              sub={
                snapshot()?.runtime_stats_available
                  ? (t('systemRuntimeOn') as string)
                  : (t('systemRuntimeOff') as string)
              }
            />
            <Stat
              label={t('systemResetReason') as string}
              value={resetReasonInfo().label}
              warn={resetReasonInfo().warn}
            />
            <Stat
              label={t('systemMinHeap') as string}
              value={snapshot() ? formatBytes(snapshot()!.min_free_heap_bytes) : '-'}
              sub={t('systemMinHeapSub') as string}
            />
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('systemSectionMemory') as string}>
          <div class="grid gap-3 sm:grid-cols-2 pt-2">
            <div class="flex flex-col gap-2 px-3 py-3 rounded-[var(--radius-sm)] bg-white/[0.02] border border-[var(--color-border-subtle)]">
              <div class="flex items-center justify-between">
                <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
                  {t('systemHeapInternal')}
                </span>
                <span class="text-[0.78rem] font-mono text-[var(--color-text-primary)]">
                  {snapshot()
                    ? `${formatBytes(snapshot()!.total_internal_bytes - snapshot()!.free_internal_bytes)} / ${formatBytes(snapshot()!.total_internal_bytes)}`
                    : '-'}
                </span>
              </div>
              <div class="h-2 w-full rounded bg-white/5 overflow-hidden">
                <div
                  class="h-full bg-[var(--color-accent)] transition-[width] duration-300"
                  style={{ width: `${Math.min(100, internalPct())}%` }}
                />
              </div>
            </div>
            <div class="flex flex-col gap-2 px-3 py-3 rounded-[var(--radius-sm)] bg-white/[0.02] border border-[var(--color-border-subtle)]">
              <div class="flex items-center justify-between">
                <span class="text-[0.78rem] uppercase tracking-wider text-[var(--color-text-muted)] font-semibold">
                  {t('systemHeapPsram')}
                </span>
                <span class="text-[0.78rem] font-mono text-[var(--color-text-primary)]">
                  {snapshot() && snapshot()!.total_psram_bytes > 0
                    ? `${formatBytes(snapshot()!.total_psram_bytes - snapshot()!.free_psram_bytes)} / ${formatBytes(snapshot()!.total_psram_bytes)}`
                    : t('systemPsramNone')}
                </span>
              </div>
              <div class="h-2 w-full rounded bg-white/5 overflow-hidden">
                <div
                  class="h-full bg-[var(--color-accent)] transition-[width] duration-300"
                  style={{ width: `${Math.min(100, psramPct())}%` }}
                />
              </div>
            </div>
          </div>
        </StaticConfigBlock>
        <StaticConfigBlock title={t('systemSectionTasks') as string}>
          <Show when={!snapshot()?.runtime_stats_available}>
            <div class="pt-2">
              <Banner kind="info" message={t('systemRuntimeStatsMissing') as string} />
            </div>
          </Show>
          <div class="pt-2 overflow-x-auto">
            <table class="w-full text-[0.82rem]">
              <thead>
                <tr class="text-left text-[var(--color-text-muted)] border-b border-[var(--color-border-subtle)]">
                  <th class="py-1 pr-3 font-semibold">{t('systemColName')}</th>
                  <th class="py-1 pr-3 font-semibold">{t('systemColState')}</th>
                  <th class="py-1 pr-3 font-semibold text-right">{t('systemColPriority')}</th>
                  <th class="py-1 pr-3 font-semibold text-right">{t('systemColCpu')}</th>
                  <th class="py-1 pr-3 font-semibold text-right">{t('systemColStack')}</th>
                  <th class="py-1 pr-3 font-semibold text-right">{t('systemColCore')}</th>
                </tr>
              </thead>
              <tbody>
                <For each={tasks()}>
                  {(tk) => (
                    <tr class="border-b border-white/[0.03] hover:bg-white/[0.02]">
                      <td class="py-1 pr-3 font-mono text-[var(--color-text-primary)]">{tk.name}</td>
                      <td class="py-1 pr-3">
                        <StateBadge state={tk.state} />
                      </td>
                      <td class="py-1 pr-3 text-right font-mono text-[var(--color-text-secondary)]">
                        {tk.priority}
                      </td>
                      <td class="py-1 pr-3 text-right font-mono text-[var(--color-text-secondary)]">
                        {snapshot()?.runtime_stats_available
                          ? `${tk.cpu_pct.toFixed(1)}%`
                          : '-'}
                      </td>
                      <td class="py-1 pr-3 text-right font-mono text-[var(--color-text-secondary)]">
                        {formatBytes(tk.stack_free_bytes)}
                      </td>
                      <td class="py-1 pr-3 text-right font-mono text-[var(--color-text-secondary)]">
                        {tk.core_id === undefined || tk.core_id < 0 ? '—' : tk.core_id}
                      </td>
                    </tr>
                  )}
                </For>
              </tbody>
            </table>
          </div>
        </StaticConfigBlock>
      </div>
    </TabShell>
  );
};
