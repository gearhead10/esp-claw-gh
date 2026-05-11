import { createMemo, createSignal, Show, type Component } from 'solid-js';
import { t } from '../i18n';
import type { AppConfig } from '../api/client';
import { createConfigTab } from '../state/configTab';
import { TabShell } from '../components/layout/TabShell';
import { PageHeader } from '../components/ui/PageHeader';
import { StaticConfigBlock } from '../components/ui/ConfigBlocks';
import { TextArea } from '../components/ui/FormField';
import { Button } from '../components/ui/Button';
import { SavePanel } from '../components/ui/SavePanel';
import { Banner } from '../components/ui/Banner';
import { RestartConfirmModal } from '../components/system/RestartConfirmModal';

type WebhookForm = {
  webhooks_json: string;
};

const EXAMPLE_ENTRY = {
  name: 'example_slack',
  url: 'https://hooks.slack.com/services/REPLACE/ME',
  method: 'POST',
  description: 'Slack incoming webhook (replace URL)',
};

const DEFAULT_TEXT = '[]';

function parseEntries(raw: string): { ok: true; entries: unknown[] } | { ok: false; error: string } {
  const trimmed = raw.trim();
  if (!trimmed) {
    return { ok: true, entries: [] };
  }
  try {
    const parsed = JSON.parse(trimmed);
    if (!Array.isArray(parsed)) {
      return { ok: false, error: 'Top-level must be an array' };
    }
    return { ok: true, entries: parsed };
  } catch (err) {
    return { ok: false, error: (err as Error).message };
  }
}

export const WebhookPage: Component<{ onRestartRequest: () => void }> = (props) => {
  const tab = createConfigTab<WebhookForm>({
    tab: 'webhook',
    groups: ['webhook'],
    toForm: (config: Partial<AppConfig>) => ({
      webhooks_json: config.webhooks_json ?? DEFAULT_TEXT,
    }),
    fromForm: (form) => ({
      webhooks_json: form.webhooks_json.trim() || DEFAULT_TEXT,
    }),
  });

  const [confirmOpen, setConfirmOpen] = createSignal(false);

  const validation = createMemo(() => parseEntries(tab.form.webhooks_json));

  const summaryCount = createMemo(() => {
    const v = validation();
    return v.ok ? v.entries.length : 0;
  });

  const handleFormat = () => {
    const v = validation();
    if (!v.ok) return;
    tab.setForm('webhooks_json', JSON.stringify(v.entries, null, 2));
  };

  const handleAddExample = () => {
    const v = validation();
    const current = v.ok ? v.entries : [];
    const next = [...current, EXAMPLE_ENTRY];
    tab.setForm('webhooks_json', JSON.stringify(next, null, 2));
  };

  const handleSave = async () => {
    const v = validation();
    if (!v.ok) {
      return;
    }
    await tab.save();
    setConfirmOpen(true);
  };

  return (
    <TabShell>
      <PageHeader
        title={t('navWebhook') as string}
        description={t('webhookNote') as string}
      />
      <Show when={tab.error()}>
        <div class="px-5 pt-4">
          <Banner kind="error" message={tab.error() ?? undefined} />
        </div>
      </Show>
      <div class="divide-y divide-[var(--color-border-subtle)] mt-2">
        <StaticConfigBlock title={t('sectionWebhook') as string}>
          <div class="pt-2 space-y-3">
            <p class="text-sm text-[var(--color-text-muted)]">{t('webhookEditorHelp') as string}</p>
            <div class="flex flex-wrap gap-2">
              <Button
                variant="ghost"
                size="sm"
                disabled={!validation().ok}
                onClick={handleFormat}
              >
                {t('webhookFormatBtn') as string}
              </Button>
              <Button variant="ghost" size="sm" onClick={handleAddExample}>
                {t('webhookAddExample') as string}
              </Button>
              <span class="text-sm text-[var(--color-text-muted)] self-center">
                {(t('webhookSummary') as string).replace('{count}', String(summaryCount()))}
              </span>
            </div>
            <TextArea
              label={t('webhookEditorLabel') as string}
              value={tab.form.webhooks_json}
              rows={14}
              spellcheck={false}
              onInput={(event) => tab.setForm('webhooks_json', event.currentTarget.value)}
            />
            <Show when={!validation().ok}>
              <Banner
                kind="error"
                message={(t('webhookInvalidJson') as string).replace(
                  '{error}',
                  (validation() as { ok: false; error: string }).error,
                )}
              />
            </Show>
          </div>
        </StaticConfigBlock>
      </div>
      <SavePanel
        dirty={tab.dirty() && validation().ok}
        saving={tab.saving()}
        onSave={() => handleSave().catch(() => undefined)}
        onDiscard={tab.discard}
        note={t('restartHint') as string}
      />
      <RestartConfirmModal
        open={confirmOpen()}
        onClose={() => setConfirmOpen(false)}
        onConfirm={() => {
          setConfirmOpen(false);
          props.onRestartRequest();
        }}
        subtitle={t('restartHint') as string}
      />
    </TabShell>
  );
};
