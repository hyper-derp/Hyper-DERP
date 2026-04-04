import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  benchmarks: [
    {type: 'doc', id: 'index', label: 'Overview'},
    'gcp',
    'latency',
    'tunnel',
    'peer-scaling',
    'bare-metal',
  ],
};

export default sidebars;
