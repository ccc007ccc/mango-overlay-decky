export interface ApplicationStatus {
  application_id: string;
  display_name: string;
  approved: boolean;
  visible: boolean;
  order: number;
  active_instances: number;
}

export interface BrokerStatus {
  enabled: boolean;
  require_approval: boolean;
  scene_revision: number;
  applications: ApplicationStatus[];
}

export interface PluginStatus {
  plugin_version: string;
  active_version: string | null;
  previous_version: string | null;
  broker: BrokerStatus | null;
  error: string | null;
  test_canvas: boolean;
}
