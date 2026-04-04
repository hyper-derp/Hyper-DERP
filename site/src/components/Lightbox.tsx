import React, {useEffect, useCallback} from 'react';

export default function Lightbox(): React.ReactElement {
  const close = useCallback(() => {
    document.getElementById('lightbox')
      ?.classList.remove('open');
  }, []);

  useEffect(() => {
    const handler = (e: KeyboardEvent) => {
      if (e.key === 'Escape') close();
    };
    document.addEventListener('keydown', handler);
    return () => document.removeEventListener(
      'keydown', handler);
  }, [close]);

  return (
    <div id="lightbox" className="lightbox"
         onClick={close}>
      <img src="" alt="" />
    </div>
  );
}
