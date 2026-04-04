import React, {useCallback} from 'react';
import {useColorMode} from '@docusaurus/theme-common';

interface PlotProps {
  src: string;
  dir?: string;
  alt?: string;
}

export default function Plot({
  src,
  dir = '/img/blog',
  alt = '',
}: PlotProps): React.ReactElement {
  const {colorMode} = useColorMode();
  const dark = `${dir}/${src}`;
  const light = `${dir}/light/${src}`;
  const current = colorMode === 'dark' ? dark : light;

  const handleClick = useCallback(
    (e: React.MouseEvent) => {
      e.preventDefault();
      const lb = document.getElementById('lightbox');
      const img = lb?.querySelector('img');
      if (lb && img) {
        img.src = current;
        img.alt = alt;
        lb.classList.add('open');
      }
    },
    [current, alt],
  );

  return (
    <a href={current} className="plot-link"
       onClick={handleClick}>
      <img src={current} alt={alt} />
    </a>
  );
}
