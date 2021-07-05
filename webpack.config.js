const path = require("path");

module.exports = {
  target: "node",
  mode: "development",
  entry: "./src/web_client/index.js",
  output: {
    filename: "main.js",
    path: path.resolve(__dirname, "docs"),
    publicPath: "docs",
  },
};
