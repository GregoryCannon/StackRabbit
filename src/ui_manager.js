/**
 * Handle UI animations that are indepedent of the main tetris game
 */
import { BOARD_HEIGHT, DISPLAY_FULL_WIDTH } from "./constants.js";

// Get elements
const mainCanvas = document.getElementById("main-canvas");
const leftPanelOpenToggle = document.getElementById("left-panel-toggle-button");
const leftPanel = document.getElementById("left-panel");
const rightPanel = document.getElementById("right-panel");
let leftPanelIsOpen = true;

leftPanel.style.minHeight = BOARD_HEIGHT + 60;

// Resize the canvas based on the square size
mainCanvas.setAttribute("height", BOARD_HEIGHT);
mainCanvas.setAttribute("width", DISPLAY_FULL_WIDTH);

leftPanelOpenToggle.innerText = "<"; // Set here b/c the < messes with the HTML auto-format
leftPanelOpenToggle.addEventListener("click", function (e) {
  leftPanelIsOpen = !leftPanelIsOpen;

  if (leftPanelIsOpen) {
    leftPanel.style.marginLeft = 0;
    leftPanelOpenToggle.innerText = "<";
  } else {
    leftPanel.style.marginLeft = -290;
    leftPanelOpenToggle.innerText = ">";
  }
});

// Configure the level select buttons
const levelSelectInput = document.getElementById("level-select");

// Assign on click listeners, e.g. #level-0 sets it to 0.
[0, 8, 9, 15, 18, 19, 29].forEach((num) => {
  document.getElementById("level-" + num).addEventListener("click", (e) => {
    levelSelectInput.value = num;
  });
});
