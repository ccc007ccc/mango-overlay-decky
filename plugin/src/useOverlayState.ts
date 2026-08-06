import { useCallback, useEffect, useRef, useState } from "react";

import {
  getStatus,
  restartBroker as restartBrokerRequest,
  setEnabled as setEnabledRequest,
  setProviderPolicy as setProviderPolicyRequest,
  setProviderPosition as setProviderPositionRequest,
  setRequireApproval as setRequireApprovalRequest,
  setTestCanvas as setTestCanvasRequest,
} from "./api";
import type { BrokerStatus, PluginStatus } from "./types";

function errorText(reason: unknown): string {
  if (reason instanceof Error && reason.message) return reason.message;
  return String(reason || "操作失败");
}

export function useOverlayState() {
  const [status, setStatus] = useState<PluginStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [pending, setPending] = useState(false);
  const refreshing = useRef(false);

  const refresh = useCallback(async () => {
    if (refreshing.current) return;
    refreshing.current = true;
    try {
      const next = await getStatus();
      setStatus(next);
      setError(next.error);
    } catch (reason) {
      setError(errorText(reason));
    } finally {
      refreshing.current = false;
    }
  }, []);

  useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 3000);
    return () => window.clearInterval(timer);
  }, [refresh]);

  const mutate = useCallback(
    async (request: () => Promise<BrokerStatus>) => {
      setPending(true);
      try {
        const broker = await request();
        setStatus((current) =>
          current ? { ...current, broker, error: null } : current,
        );
        setError(null);
      } catch (reason) {
        setError(errorText(reason));
        await refresh();
      } finally {
        setPending(false);
      }
    },
    [refresh],
  );

  const setEnabled = useCallback(
    (enabled: boolean) => mutate(() => setEnabledRequest(enabled)),
    [mutate],
  );
  const setRequireApproval = useCallback(
    (required: boolean) => mutate(() => setRequireApprovalRequest(required)),
    [mutate],
  );
  const setProviderPolicy = useCallback(
    (applicationId: string, approved: boolean, visible: boolean, order: number) =>
      mutate(() =>
        setProviderPolicyRequest(applicationId, approved, visible, order),
      ),
    [mutate],
  );
  const setProviderPosition = useCallback(
    (applicationId: string, position: number) =>
      mutate(() => setProviderPositionRequest(applicationId, position)),
    [mutate],
  );
  const setTestCanvas = useCallback(async (enabled: boolean) => {
    setPending(true);
    try {
      const active = await setTestCanvasRequest(enabled);
      setStatus((current) =>
        current ? { ...current, test_canvas: active } : current,
      );
      setError(null);
    } catch (reason) {
      setError(errorText(reason));
      await refresh();
    } finally {
      setPending(false);
    }
  }, [refresh]);
  const restartBroker = useCallback(async () => {
    setPending(true);
    try {
      await restartBrokerRequest();
      setError(null);
      await refresh();
    } catch (reason) {
      setError(errorText(reason));
    } finally {
      setPending(false);
    }
  }, [refresh]);

  return {
    error,
    pending,
    refresh,
    restartBroker,
    setEnabled,
    setProviderPolicy,
    setProviderPosition,
    setRequireApproval,
    setTestCanvas,
    status,
  };
}
