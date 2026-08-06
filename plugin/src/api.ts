import { callable } from "@decky/api";

import type { BrokerStatus, PluginStatus } from "./types";

export const getStatus = callable<[], PluginStatus>("get_status");
export const setEnabled = callable<[enabled: boolean], BrokerStatus>("set_enabled");
export const setRequireApproval = callable<[required: boolean], BrokerStatus>(
  "set_require_approval",
);
export const setProviderPolicy = callable<
  [applicationId: string, approved: boolean, visible: boolean, order: number],
  BrokerStatus
>("set_provider_policy");
export const setProviderPosition = callable<
  [applicationId: string, position: number],
  BrokerStatus
>("set_provider_position");
export const setTestCanvas = callable<[enabled: boolean], boolean>("set_test_canvas");
export const restartBroker = callable<[], void>("restart_broker");
