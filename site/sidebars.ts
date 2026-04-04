import type {SidebarsConfig} from
  '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  docs: [
    {
      type: 'category',
      label: 'Getting Started',
      items: [
        'install',
        'configuration',
        'operations',
      ],
    },
    {
      type: 'category',
      label: 'Reference',
      items: [
        'architecture',
        'building',
        'troubleshooting',
      ],
    },
  ],
};

export default sidebars;
