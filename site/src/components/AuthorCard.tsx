import React from 'react';

interface AuthorCardProps {
  name: string;
  email?: string;
  date: string;
  photo?: string;
}

export default function AuthorCard({
  name, email, date, photo,
}: AuthorCardProps): React.ReactElement {
  return (
    <div className="author-card">
      <div className="author-photo">
        {photo ? (
          <img src={photo} alt={name} />
        ) : (
          <span className="author-initials">
            {name.charAt(0)}
          </span>
        )}
      </div>
      <div className="author-info">
        <span className="author-name">{name}</span>
        {email && (
          <a href={`mailto:${email}`}
             className="author-email">
            {email}
          </a>
        )}
        <time>{date}</time>
      </div>
    </div>
  );
}
