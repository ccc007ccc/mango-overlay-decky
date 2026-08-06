import { definePlugin } from "@decky/api";
import { staticClasses } from "@decky/ui";
import { FaLayerGroup } from "react-icons/fa6";

import { Panel } from "./Panel";

export default definePlugin(() => ({
  name: "Mango Overlay",
  titleView: <div className={staticClasses.Title}>Mango Overlay</div>,
  content: <Panel />,
  icon: <FaLayerGroup />,
}));
