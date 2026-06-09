const { spawn } = require('child_process');

function runCommand(command, args, label) {
  return new Promise((resolve, reject) => {
    const proc = spawn(command, args, { shell: true });

    proc.stdout.on('data', (data) => {
      process.stdout.write(`[${label}] ${data}`);
    });

    proc.stderr.on('data', (data) => {
      const message = data.toString();
      process.stderr.write(`[${label}] ${data}`);

      // Highlight node-gyp errors
      if (message.includes('node-gyp') && (message.includes('python') || message.includes('C++'))) {
        console.error('\n❌ Detected node-gyp error. Make sure Python and a C++ build environment are installed.');
        console.error('👉 For Windows, install Python here: https://www.python.org/downloads/');
        console.error('And install VS Studio 2022 here: https://visualstudio.microsoft.com/downloads/');
        console.error('👉 For macOS, run: xcode-select --install');
        console.error('👉 For Linux, install: python3, make, and g++');
      }
    });

    proc.on('close', (code) => {
      if (code !== 0) {
        console.error(`\n⚠️ ${label} exited with code ${code}`);
        reject(new Error(`${label} failed`));
      } else {
        resolve();
      }
    });
  });
}

async function installDependencies() {
  try {
    console.log('📦 Installing node-gyp...');
    await runCommand('npm', ['install', 'node-gyp'], 'node-gyp');

    console.log('📦 Installing nan...');
    await runCommand('npm', ['install', 'nan'], 'nan');

    console.log('📦 Running project npm install...');
    await runCommand('npm', ['install'], 'npm-install');

    console.log('\n✅ All dependencies installed successfully!');
  } catch (err) {
    console.error(`\n❌ Installation failed: ${err.message}`);
  }
}

installDependencies();
