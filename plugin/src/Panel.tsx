import {
  ButtonItem,
  DropdownItem,
  Field,
  PanelSection,
  PanelSectionRow,
  SteamSpinner,
  Toggle,
  ToggleField,
} from "@decky/ui";
import { FaRotate } from "react-icons/fa6";

import type { ApplicationStatus } from "./types";
import { useOverlayState } from "./useOverlayState";

function Notice({ children }: { children: string }) {
  return (
    <div
      style={{
        borderLeft: "3px solid #f26d78",
        margin: "8px 0 12px",
        overflowWrap: "anywhere",
        padding: "8px 10px",
      }}
    >
      {children}
    </div>
  );
}

function ProviderControls({
  application,
  applications,
  disabled,
  onPolicy,
  onPosition,
}: {
  application: ApplicationStatus;
  applications: ApplicationStatus[];
  disabled: boolean;
  onPolicy: (approved: boolean, visible: boolean) => void;
  onPosition: (position: number) => void;
}) {
  const position = applications.findIndex(
    (item) => item.application_id === application.application_id,
  );
  const positions = applications.map((_, index) => ({
    data: index,
    label:
      index === 0
        ? "1（底层）"
        : index === applications.length - 1
          ? `${index + 1}（顶层）`
          : String(index + 1),
  }));
  return (
    <PanelSection title={application.display_name}>
      <PanelSectionRow>
        <Field label={application.application_id}>
          <span style={{ opacity: 0.72 }}>
            {application.active_instances > 0
              ? `${application.active_instances} 个实例`
              : "未连接"}
          </span>
        </Field>
        <ToggleField
          checked={application.approved}
          disabled={disabled}
          label="授权"
          onChange={(approved) => onPolicy(approved, application.visible)}
        />
        <ToggleField
          checked={application.visible}
          disabled={disabled || !application.approved}
          label="显示"
          onChange={(visible) => onPolicy(application.approved, visible)}
        />
        <DropdownItem
          disabled={disabled || applications.length < 2}
          label="层级"
          rgOptions={positions}
          selectedOption={position}
          strDefaultLabel={String(position + 1)}
          onChange={(option) => onPosition(Number(option.data))}
        />
      </PanelSectionRow>
    </PanelSection>
  );
}

export function Panel() {
  const state = useOverlayState();
  if (!state.status) {
    return (
      <PanelSection>
        <PanelSectionRow>
          <SteamSpinner background="transparent" />
          {state.error ? <Notice>{state.error}</Notice> : null}
        </PanelSectionRow>
      </PanelSection>
    );
  }

  const broker = state.status.broker;
  const providers =
    broker?.applications.filter(
      (application) => application.application_id !== "dev.mango-overlay.test",
    ) ?? [];
  return (
    <div style={{ minWidth: 0, paddingBottom: "16px", width: "100%" }}>
      {state.error ? <Notice>{state.error}</Notice> : null}
      <PanelSection>
        <PanelSectionRow>
          <Field label="第三方画布">
            <Toggle
              disabled={!broker || state.pending}
              value={broker?.enabled ?? false}
              onChange={(enabled) => void state.setEnabled(enabled)}
            />
          </Field>
          <Field label="运行时">
            <span style={{ opacity: 0.72 }}>
              {state.status.active_version ?? "不可用"}
            </span>
          </Field>
        </PanelSectionRow>
      </PanelSection>
      {broker ? (
        <>
          <PanelSection title="授权">
            <PanelSectionRow>
              <ToggleField
                checked={broker.require_approval}
                disabled={state.pending}
                label="新提供者需批准"
                onChange={(required) => void state.setRequireApproval(required)}
              />
            </PanelSectionRow>
          </PanelSection>
          {providers.map((application) => (
            <ProviderControls
              key={application.application_id}
              application={application}
              applications={providers}
              disabled={state.pending}
              onPolicy={(approved, visible) =>
                void state.setProviderPolicy(
                  application.application_id,
                  approved,
                  visible,
                  application.order,
                )
              }
              onPosition={(position) =>
                void state.setProviderPosition(application.application_id, position)
              }
            />
          ))}
          <PanelSection title="诊断">
            <PanelSectionRow>
              <ToggleField
                checked={state.status.test_canvas}
                disabled={state.pending}
                label="测试画布"
                onChange={(enabled) => void state.setTestCanvas(enabled)}
              />
              <ButtonItem
                bottomSeparator="none"
                disabled={state.pending}
                layout="below"
                onClick={() => void state.restartBroker()}
              >
                <span
                  style={{
                    alignItems: "center",
                    display: "flex",
                    gap: "8px",
                    justifyContent: "center",
                  }}
                >
                  <FaRotate aria-hidden="true" />
                  重启场景服务
                </span>
              </ButtonItem>
            </PanelSectionRow>
          </PanelSection>
        </>
      ) : null}
      <PanelSection>
        <PanelSectionRow>
          <ButtonItem
            bottomSeparator="none"
            disabled={state.pending}
            layout="below"
            onClick={() => void state.refresh()}
          >
            <span
              style={{
                alignItems: "center",
                display: "flex",
                gap: "8px",
                justifyContent: "center",
              }}
            >
              <FaRotate aria-hidden="true" />
              刷新
            </span>
          </ButtonItem>
        </PanelSectionRow>
      </PanelSection>
    </div>
  );
}
