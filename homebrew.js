const GAME_EXTENSIONS = [

  '.ffpfsc',

  '.ffpfs',

  '.pfs',

  '.ffpkg',

  '.ufs',

  '.exfat',

  '.img',

];



const DEFAULT_GAMES_DIR = '/data/homebrew';



function isGameFile(name) {

  const lower = name.toLowerCase();

  return GAME_EXTENSIONS.some((ext) => lower.endsWith(ext));

}



function buildScanLocations() {

  const locations = [

    '/data/homebrew',

    '/data/etaHEN/games',

    '/mnt/ext0/homebrew',

    '/mnt/ext0/etaHEN/games',

    '/mnt/ext1/homebrew',

    '/mnt/ext1/etaHEN/games',

    '/mnt/ext0',

    '/mnt/ext1',

    '/mnt/shadowmnt/pfsc',

    '/mnt/shadowmnt',

    '/mnt/shadowmount/pfsc',

    '/mnt/shadowmount',

  ];



  for (let i = 0; i <= 7; i++) {

    locations.push(`/mnt/usb${i}/homebrew`);

    locations.push(`/mnt/usb${i}/etaHEN/games`);

    locations.push(`/mnt/usb${i}`);

  }



  return locations;

}



function formatMntDeviceName(name) {

  const isUsb = name.startsWith('usb');

  const isExt = name.startsWith('ext');

  if (!isUsb && !isExt) return name;



  if (name === 'ext0') return 'USB Extended Storage';

  if (name === 'ext1') return 'M.2 SSD Storage';



  const num = parseInt(name.substring(3), 10);

  if (Number.isNaN(num)) return name;

  return (isUsb ? 'USB Storage ' : 'PS Formatted Storage ') + (num + 1);

}



function formatShortcutLabel(path) {

  if (path === '/data/homebrew') return 'Internal · homebrew';

  if (path === '/data/etaHEN/games') return 'Internal · etaHEN games';

  if (path === '/mnt/shadowmnt/pfsc' || path === '/mnt/shadowmount/pfsc') {

    return 'Shadow mount · PFSC';

  }

  if (path === '/mnt/shadowmnt' || path === '/mnt/shadowmount') {

    return 'Shadow mount';

  }



  const extMatch = path.match(/^\/mnt\/(ext\d+)(?:\/(.*))?$/);

  if (extMatch) {

    const drive = extMatch[1];

    const rest = extMatch[2];

    if (rest === 'homebrew') return `${formatMntDeviceName(drive)} · homebrew`;

    if (rest === 'etaHEN/games') return `${formatMntDeviceName(drive)} · etaHEN games`;

    if (!rest) return formatMntDeviceName(drive);

  }



  const usbMatch = path.match(/^\/mnt\/(usb\d+)(?:\/(.*))?$/);

  if (usbMatch) {

    const drive = usbMatch[1];

    const rest = usbMatch[2];

    if (rest === 'homebrew') return `${formatMntDeviceName(drive)} · homebrew`;

    if (rest === 'etaHEN/games') return `${formatMntDeviceName(drive)} · etaHEN games`;

    if (!rest) return formatMntDeviceName(drive);

  }



  return path;

}



async function listMntDevices() {

  const res = await ApiClient.fsListDir('/mnt/');

  if (res.status !== 200 || !res.data) {

    return [];

  }



  const devices = [];

  for (const entry of res.data) {

    if (entry.mode !== 'm') {

      continue;

    }



    const isUsb = entry.name.startsWith('usb');

    const isExt = entry.name.startsWith('ext');

    if (!isUsb && !isExt) {

      continue;

    }



    const num = parseInt(entry.name.substring(3), 10);

    if (Number.isNaN(num)) {

      continue;

    }



    devices.push({

      label: formatMntDeviceName(entry.name),

      path: `/mnt/${entry.name}/`,

    });

  }



  return devices;

}



function isRedundantShortcut(shortcut, devices) {

  const normalized = shortcut.path.replace(/\/$/, '');

  return devices.some((dev) => dev.path.replace(/\/$/, '') === normalized);

}



async function scanDirInstallables(path) {

  const res = await ApiClient.fsListDir(path);

  if (res.status !== 200 || !res.data) {

    return { hasGames: false, count: 0 };

  }



  let count = 0;

  const entries = res.data;



  if (entries.some((entry) => entry.isDir() && entry.name === 'sce_sys')) {

    count++;

  }



  for (const entry of entries) {

    if (entry.isFile() && isGameFile(entry.name)) {

      count++;

      continue;

    }



    if (!entry.isDir() || entry.name.startsWith('.')) {

      continue;

    }



    const subRes = await ApiClient.fsListDir(`${path}/${entry.name}`);

    if (subRes.status !== 200 || !subRes.data) {

      continue;

    }



    const hasImage = subRes.data.some((item) => item.isFile() && isGameFile(item.name));

    const hasSceSys = subRes.data.some((item) => item.isDir() && item.name === 'sce_sys');

    if (hasImage || hasSceSys) {

      count++;

    }

  }



  return { hasGames: count > 0, count };

}



async function scanForShortcuts() {

  const locations = buildScanLocations();

  const scans = await Promise.all(

    locations.map(async (path) => {

      try {

        const result = await scanDirInstallables(path);

        if (!result.hasGames) {

          return null;

        }

        return {

          path,

          label: formatShortcutLabel(path),

          count: result.count,

        };

      } catch (_err) {

        return null;

      }

    }),

  );



  return scans.filter(Boolean);

}



function launchConfig(cwd) {

  return {

    path: `${window.workingDir}/dump_installer.elf`,

    cwd,

    daemon: true,

  };

}



function normalizePickPath(path) {

  if (!path || typeof path !== 'string') {

    return null;

  }



  const trimmed = path.trim();

  if (!trimmed) {

    return null;

  }



  if (trimmed !== '/' && trimmed.endsWith('/')) {

    return trimmed.slice(0, -1);

  }



  return trimmed;

}



function isUnsafeInstallCwd(cwd) {

  const normalized = normalizePickPath(cwd);

  if (!normalized) {

    return true;

  }



  if (normalized === '/') {

    return true;

  }



  return false;

}



async function prepareInstallLaunch(cwd) {

  const normalized = normalizePickPath(cwd);

  if (isUnsafeInstallCwd(normalized)) {

    alert('Select a folder that contains games (not the console root).');

    return {};

  }



  const scanPath = normalized.endsWith('/') ? normalized : `${normalized}/`;

  const scan = await scanDirInstallables(scanPath);

  if (!scan.hasGames) {

    alert('No installable games found in that folder.');

    return {};

  }



  return launchConfig(scanPath);

}



async function openStoragePicker() {

  const cwd = await pickDirectory(DEFAULT_GAMES_DIR, 'Select Game Directory...');

  return cwd ? prepareInstallLaunch(cwd) : {};

}



async function main() {

  const shortcuts = await Promise.race([

    scanForShortcuts(),

    new Promise((resolve) => setTimeout(() => resolve([]), 4000)),

  ]);



  return {

    mainText: 'Dump Installer',

    secondaryText: 'By EchoStretch',

    onclick: openStoragePicker,

    options: [

      ...shortcuts.map((shortcut) => ({

        text: `${shortcut.label} (${shortcut.count})`,

        onclick: async () => prepareInstallLaunch(shortcut.path),

      })),

      {

        text: 'Autoload — scan all storage',

        onclick: async () => {

          await ApiClient.launchApp(

            `${window.workingDir}/dump_installer.elf`,

            ['--autoload'],

            null,

            window.workingDir,

            true,

          );

          return {};

        },

      },

      {

        text: 'Remove grey icons (pick games folder)',

        onclick: async () => {

          const cwd = await pickDirectory(DEFAULT_GAMES_DIR, 'Select folder with .ffpfsc files...');

          if (!cwd) {

            return {};

          }



          const normalized = normalizePickPath(cwd);

          await ApiClient.launchApp(

            `${window.workingDir}/dump_installer.elf`,

            ['--purge-grey'],

            null,

            normalized,

            true,

          );

          return {};

        },

      },

      {

        text: 'Remove /data/dilocs folder only',

        onclick: async () => {

          await ApiClient.launchApp(

            `${window.workingDir}/dump_installer.elf`,

            ['--cleanup-dilocs'],

            null,

            window.workingDir,

            true,

          );

          return {};

        },

      },

    ],

  };

}


