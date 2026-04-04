import React from 'react';
import Tabs from '@theme/Tabs';
import TabItem from '@theme/TabItem';
import CodeBlock from '@theme/CodeBlock';

const aptCode = `curl -fsSL https://hyper-derp.dev/repo/key.gpg | \\
  sudo gpg --dearmor -o /usr/share/keyrings/hyper-derp.gpg

echo "deb [signed-by=/usr/share/keyrings/hyper-derp.gpg] \\
  https://hyper-derp.dev/repo stable main" | \\
  sudo tee /etc/apt/sources.list.d/hyper-derp.list

sudo apt update && sudo apt install hyper-derp`;

const sourceCode = `sudo apt install cmake ninja-build clang \\
  liburing-dev libsodium-dev libspdlog-dev libssl-dev

git clone https://github.com/hyper-derp/hyper-derp.git
cd hyper-derp

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build`;

export default function InstallTabs(): React.ReactElement {
  return (
    <Tabs defaultValue="apt" values={[
      {label: 'APT Repository', value: 'apt'},
      {label: 'Build from Source', value: 'source'},
    ]}>
      <TabItem value="apt">
        <CodeBlock language="bash">{aptCode}</CodeBlock>
      </TabItem>
      <TabItem value="source">
        <CodeBlock language="bash">
          {sourceCode}
        </CodeBlock>
      </TabItem>
    </Tabs>
  );
}
