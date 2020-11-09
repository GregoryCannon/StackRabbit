/**
 * Handle UI animations that are indepedent of the main tetris game
 */
import {
  NUM_ROW,
  NUM_COLUMN,
  SQUARE_SIZE,
  BOARD_HEIGHT,
  BOARD_WIDTH,
} from "./constants.js";

// Get elements
const mainCanvas = document.getElementById("main-canvas");
const leftPanelOpenToggle = document.getElementById("left-panel-toggle-open");
const leftPanel = document.getElementById("left-panel");
const rightPanel = document.getElementById("right-panel");
let leftPanelIsOpen = true;

leftPanel.style.minHeight = BOARD_HEIGHT + 60;

// Resize the canvas based on the square size
mainCanvas.setAttribute("height", BOARD_HEIGHT);
mainCanvas.setAttribute("width", BOARD_WIDTH);

leftPanelOpenToggle.addEventListener("click", function (e) {
  leftPanelIsOpen = !leftPanelIsOpen;

  if (leftPanelIsOpen) {
    leftPanel.style.marginLeft = 0;
  } else {
    leftPanel.style.marginLeft = -260;
  }
});
