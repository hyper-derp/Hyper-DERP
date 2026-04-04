import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

const config: Config = {
  title: 'Hyper-DERP',
  tagline: 'High-performance DERP relay server',
  favicon: 'img/favicon.ico',

  url: 'https://hyper-derp.dev',
  baseUrl: '/',
  trailingSlash: true,

  organizationName: 'hyper-derp',
  projectName: 'Hyper-DERP',

  onBrokenLinks: 'throw',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  stylesheets: [
    {
      href: 'https://fonts.googleapis.com/css2?family=Fira+Code:wght@400;500;600&family=Fira+Sans:ital,wght@0,400;0,500;0,600;0,700;1,400&display=swap',
      type: 'text/css',
    },
  ],

  plugins: [
    [
      '@docusaurus/plugin-content-docs',
      {
        id: 'benchmarks',
        path: 'benchmarks',
        routeBasePath: 'benchmarks',
        sidebarPath: './sidebarsBenchmarks.ts',
      },
    ],
  ],

  presets: [
    [
      'classic',
      {
        docs: {
          path: 'docs',
          routeBasePath: 'docs',
          sidebarPath: './sidebars.ts',
        },
        blog: {
          path: 'blog',
          routeBasePath: 'blog',
          blogSidebarCount: 0,
          showReadingTime: false,
        },
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    colorMode: {
      defaultMode: 'dark',
      respectPrefersColorScheme: false,
    },
    navbar: {
      title: 'Hyper-DERP',
      items: [
        {
          to: '/docs/architecture/',
          label: 'Docs',
          position: 'left',
        },
        {
          to: '/install/',
          label: 'Install',
          position: 'left',
        },
        {
          to: '/benchmarks/',
          label: 'Benchmarks',
          position: 'left',
        },
        {
          to: '/blog/',
          label: 'Blog',
          position: 'left',
        },
        {
          href: 'https://github.com/hyper-derp/Hyper-DERP',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      copyright: 'Hyper-DERP is open-source software. Source on <a href="https://github.com/hyper-derp/Hyper-DERP">GitHub</a>.',
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['bash', 'toml', 'ini', 'yaml'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
