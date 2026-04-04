import React from 'react';
import Lightbox from '@site/src/components/Lightbox';

export default function Root({
  children,
}: {
  children: React.ReactNode;
}): React.ReactElement {
  return (
    <>
      {children}
      <Lightbox />
    </>
  );
}
